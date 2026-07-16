# Feature Specification: Pattern Editor Completion (Length Dialog, Mute/Solo, View Schemes)

**Feature Branch**: `010-pattern-editor-completion`

**Created**: 2026-07-09

**Status**: Draft

**Input**: User description: "Pattern editor completion — the remaining F2 surface deferred from feature 009: (1) the pattern-length resize dialog; (2) in-F2 channel mute/solo keys; (3) the multi-scheme pattern views (Ctrl-0..5 / Ctrl-Shift-1..4) and the related view toggles, all ported faithfully from IT_PE.ASM and its data tables."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Resize a pattern from the editor (Priority: P1)

A musician composing in the pattern editor needs patterns of different lengths
(a 32-row intro, a 128-row breakdown). Today the port fixes every pattern at
its loaded length and new patterns at 64 rows; the original's set-pattern-length
requester (Ctrl-F2, `PE_SetPatternLength` / `O1_SetPatternLength`) is missing.
The user presses Ctrl-F2 in the pattern editor, is shown the original dialog
(current length primed, plus the start/end pattern range fields), enters a new
length, confirms, and the current pattern (or the selected pattern range) is
resized with the original's exact semantics: rows beyond the new length are
discarded, added rows are empty, and the change is undoable.

**Why this priority**: This is the only remaining *functional* hole in basic
composing workflow — without it a user cannot author songs whose patterns
differ from their initial length.

**Independent Test**: Open a module, press Ctrl-F2, resize the current pattern
down and back up; verify row count, data truncation/blank fill, undo
restoration, and that a saved module round-trips with the new lengths.

**Acceptance Scenarios**:

1. **Given** a 64-row pattern, **When** the user opens the length dialog and
   sets 32, **Then** the pattern becomes 32 rows, rows 32..63 are discarded,
   and the grid/row indicator reflect the new length immediately.
2. **Given** a 32-row pattern, **When** the user sets 128, **Then** the pattern
   becomes 128 rows with rows 32..127 empty and playback plays the full 128
   rows.
3. **Given** a resize that discarded data, **When** the user invokes undo
   (Ctrl-Backspace requester), **Then** the pattern is restored byte-exact to
   its prior length and contents.
4. **Given** a start/end pattern range covering several patterns, **When** the
   user confirms, **Then** every pattern in the range is resized exactly as the
   original does.
5. **Given** an out-of-bounds value, **When** the user confirms, **Then** the
   dialog clamps/rejects exactly as the original's field bounds dictate.

---

### User Story 2 - Mute and solo channels while editing (Priority: P2)

While editing in F2, the user wants to audition subsets of channels without
leaving the pattern editor. The original's key family works entirely in F2:
`\` or Alt-F9 toggles the current channel's mute, keypad `/` and `?` mute and
step to the next/previous channel, `|` solos the current channel and steps to
the next, Alt-`\` unmutes all channels, Alt-F10 solos the current channel.
Muted channels show their header in the muted colour (already rendered by the
grid) and stop sounding, consistent with the existing mute/solo controls on the
info page (F5) and order screen (F11).

**Why this priority**: Core auditioning workflow used constantly while
composing; the engine calls already exist, only the F2 key surface is missing.

**Independent Test**: In F2, toggle mute/solo with each key and verify header
colour, audible state, and agreement with the F5/F11 mute indicators.

**Acceptance Scenarios**:

1. **Given** an unmuted channel under the cursor, **When** the user presses
   `\` (or Alt-F9), **Then** that channel is muted, its header renders in the
   muted attribute, and F5/F11 agree.
2. **Given** several muted channels, **When** the user presses Alt-`\`,
   **Then** all channels are unmuted.
3. **Given** the cursor on channel 3, **When** the user presses Alt-F10 (or
   `|`), **Then** only channel 3 is audible; `|` additionally advances the
   cursor to the next channel as the original does.
4. **Given** the cursor on channel n, **When** the user presses keypad `/`
   or `?`, **Then** the original's mute-and-advance (next/previous) behaviour
   occurs.

---

### User Story 3 - Multi-scheme pattern views and view toggles (Priority: P3)

A user with many channels wants to see more tracks at once, or give different
channels different widths (e.g. full detail on the melody, tiny note-only view
on percussion). The original provides five per-channel view methods — full
(13 columns), compressed (10), all-small (7), note-only (3), tiny (2) — driven
by the `ViewChannels` table: Ctrl-1..5 assign methods, Ctrl-0 restores the
default full view, and Ctrl-Shift-1..4 apply the original's preset scheme
assignments. The channel headers shrink through the original's narrower
caption variants (" Channel xx ", "Chnl xx", "xx", …) as the width drops.
The related view behaviours also become real toggles with their original keys
and status messages: row hilight on/off, view-channel cursor tracking, and
centralise-cursor (the port already has centralise; it must join the same
toggle family), plus the track-division rendering the view tables dictate.

**Why this priority**: The largest remaining fidelity gap and a real usability
win for dense modules, but composing is possible without it.

**Independent Test**: Cycle each Ctrl-key scheme on a reference module and
compare `ITED_SHOT` captures against the original's rendering; verify toggles
flash their original status messages and change behaviour.

**Acceptance Scenarios**:

1. **Given** the default view, **When** the user presses Ctrl-2..5 on a
   channel, **Then** that channel renders in the chosen narrower method and
   more channels fit on screen; Ctrl-0/Ctrl-1 restore full width.
2. **Given** any mixed-scheme view, **When** cells are edited, **Then** the
   cursor remains addressable in every column of every visible channel and
   entry behaves identically to the full view.
3. **Given** the preset combinations Ctrl-Shift-1..4, **When** invoked,
   **Then** the channel/method assignment matches the original's presets.
4. **Given** each view toggle key, **When** pressed, **Then** the exact
   original status message ("Row hilight enabled", "View-Channel cursor
   tracking disabled", …) flashes and the behaviour changes accordingly.
5. **Given** a narrow view method, **Then** the channel caption uses the
   original's width-matched header string variant and the track division
   rendering follows the view tables.

---

### Edge Cases

- Resizing the pattern currently being played: the change must be serialised
  against the audio thread (engine lock) and playback must not read freed rows.
- Resizing to the current length (no-op) must not push a spurious undo entry
  unless the original does.
- Undo of a multi-pattern range resize must restore exactly what the
  original's undo restores (single-pattern snapshot semantics per the 10-slot
  ring's type codes).
- Solo when all channels are already muted; unmute-all when none are muted.
- Mixed view schemes where the summed widths exceed the grid: horizontal
  scrolling and the channel-notch indicator must follow the original's
  clamping (`ViewWidth`/`StartChannelEdit` behaviour).
- Block marking, paste and template entry across channels with different view
  methods must keep operating on the underlying cells (view is display-only).
- View scheme table persists per channel (100 entries) — switching patterns or
  loading a new module must reset/retain it exactly as the original does.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The pattern editor MUST provide the original set-pattern-length
  requester on its original key (Ctrl-F2), with the original dialog objects,
  field bounds, priming (current length; start/end pattern range) and
  confirm/cancel behaviour.
- **FR-002**: Applying a new length MUST preserve rows below the new length
  byte-exact, discard rows beyond it, append empty rows when growing, update
  all row-count-dependent UI, and remain safe against concurrent playback.
- **FR-003**: A length change MUST be captured in the existing 10-slot undo
  ring with the original's snapshot type/behaviour and be revertible from the
  Ctrl-Backspace requester.
- **FR-004**: The F2 key layer MUST implement the full original mute/solo
  family — mute current (`\`, Alt-F9), mute-and-advance next/previous
  (keypad `/`, `?`), solo (Alt-F10), solo-goto-next (`|`), unmute all
  (Alt-`\`) — using the engine's existing mute/solo state so F2, F5 and F11
  stay consistent, with muted headers rendered in the muted attribute.
- **FR-005**: The pattern grid renderer MUST become view-scheme driven: each
  channel renders by its assigned view method (5 methods, widths 13/10/7/3/2)
  from the per-channel view table, including the original's cell formats,
  width-matched channel captions, track division and cursor addressing for
  every method.
- **FR-006**: The original view-scheme keys MUST work: Ctrl-0..5 method
  assignment/reset and Ctrl-Shift-1..4 preset scheme application, matching the
  original handlers' semantics (including any current-channel vs all-channel
  distinctions the ASM implements).
- **FR-007**: The view toggles MUST be implemented with their original keys,
  state variables and status messages: row hilight enable/disable,
  view-channel cursor tracking, centralise cursor, and division/width
  behaviour, persisted in the editor config alongside existing preferences
  exactly as far as the original persists them.
- **FR-008**: All layout, colours, captions, dialog coordinates, bounds and
  messages MUST be taken from the original ASM data tables (IT_PE.ASM,
  IT_OBJ*.ASM), not approximated; any unavoidable deviation MUST be recorded
  in the README fidelity notes and handoff.
- **FR-009**: Engine source files MUST remain untouched; all changes are
  editor-side. Pattern data changes go through the exact pack/unpack codec and
  the engine lock.
- **FR-010**: The editor selftest MUST gain coverage for: a length shrink/grow
  round-trip with undo revert byte-equality, mute/solo state agreement across
  F2/F5/F11, and a mixed-scheme render smoke check (cursor addressing across
  methods), all leaving loaded song data untouched.

### Key Entities

- **Pattern length**: per-pattern row count; source of truth for the grid,
  playback and the packed pattern data.
- **View method**: one of five per-channel rendering schemes with fixed cell
  width and format.
- **View channel table**: the per-channel method assignment (100 entries)
  driving the renderer and cursor geometry.
- **Undo snapshot**: existing 10-slot typed ring entry capturing a pattern's
  full contents and length.
- **Channel mute state**: existing engine per-channel flag shared by F2, F5
  and F11.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A user can change any pattern's length from the pattern editor
  and immediately continue editing/playing; a shrink followed by undo restores
  the pattern byte-exact (verified by the selftest).
- **SC-002**: All four regression modules still render byte-identical audio
  (determinism gate ×4 IDENTICAL) and `--roundtrip` stays green after the
  renderer rework.
- **SC-003**: Every mute/solo key produces the same audible/visible state as
  the equivalent F5/F11 operation; no state divergence is observable.
- **SC-004**: `ITED_SHOT` captures of each view method and of the four preset
  schemes visually match the original's rendering (cell formats, captions,
  divisions) on side-by-side comparison.
- **SC-005**: The full editor selftest (including the new PE-completion
  checks) passes headless on a clean build.

## Assumptions

- The five view methods and their widths (13/10/7/3/2) and the preset-scheme
  key mapping decoded from `IT_PE.ASM` are the complete 2.17 view surface;
  exact per-method cell formats and the Ctrl-Shift preset contents will be
  decoded from the handlers during planning.
- The existing 10-slot undo ring's snapshot format already captures pattern
  length (it stores whole patterns), so no undo-format change is expected.
- Mute/solo reuses `Music_ToggleChannel`-family engine entry points already
  used by F5/F11; no new engine code is required (engine stays untouched).
- MIDI-trigger keys and MIDI input remain out of scope (as in feature 009).
- The muted-header attribute (10h) rendering already exists in the grid and
  only needs to be driven by the real state.
