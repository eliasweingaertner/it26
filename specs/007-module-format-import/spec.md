# Feature Specification: Import S3M and Other Module Formats

**Feature Branch**: `007-module-format-import`

**Created**: 2026-06-16

**Status**: Draft

**Input**: User description: "Importing S3M and other module formats. The original IT did this natively — `IT_D_RM.INC` has whole-module loaders for IT, S3M, XM, MOD, MTM and 669 (`D_LoadS3M`, `D_LoadXM`, `D_LoadMOD`, `D_LoadMTM`, `D_Load669`, `D_LoadIT`), each dispatched from the load requester (`D_LoadFile*Module`) and converted into the same song via `D_PreLoadModule`/`D_PostLoadModule`. This port's loader currently accepts only .IT ('IMPM'). Port IT's existing importers so the user can open any of those formats and have it converted into the IT in-memory song to play and edit."

**Source of truth**: The original loaders in `impulse-tracker-jthlim/IT_D_RM.INC`
(`D_LoadS3M`, `D_LoadXM`, `D_LoadMOD`, `D_LoadMTM`, `D_Load669`, `D_LoadIT`) and
their requester dispatch in `IT_DISK.ASM`. Conversion behaviour MUST match IT's
own, not a freshly-invented mapping.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Open and play an S3M module (Priority: P1)

A user opens a Scream Tracker 3 (`.S3M`) module through the load requester and
the editor converts it into the IT in-memory song so it plays through the engine
and can be edited like any loaded `.IT`. Today opening a non-`.IT` file fails the
"IMPM" signature check.

**Why this priority**: S3M is the format the user explicitly named and the
closest sibling to IT (IT was built to supersede S3M), so it is the highest-value,
most tractable first format. Delivered alone, it already lets a whole class of
modules into the editor.

**Independent Test**: Open a known-good `.S3M` file, confirm it loads without
error, plays through the engine, and that its orders/patterns/samples/instruments
appear correctly in the editor screens; render it offline and confirm stable,
sensible audio.

**Acceptance Scenarios**:

1. **Given** an `.S3M` file, **When** the user opens it, **Then** it is converted
   into the IT in-memory song (orders, patterns, samples, instruments,
   channel/pan setup) and plays through the engine.
2. **Given** the converted module, **When** the user views the editor screens,
   **Then** patterns, samples and song variables display correctly.
3. **Given** the converted module, **When** rendered offline, **Then** it produces
   stable audio that reflects the S3M's notes/effects mapped to IT semantics.

---

### User Story 2 - Import the remaining native formats (XM, MOD, MTM, 669) (Priority: P2)

A user opens the other formats IT supported natively — FastTracker `.XM`
(including its instruments and volume/pan envelopes), ProTracker `.MOD`,
MultiTracker `.MTM`, and Composer 669 `.669` — and each is converted into the IT
song for playback and editing, ported from IT's own loaders.

**Why this priority**: Broadening to IT's full native set multiplies the
feature's value, but each loader is a separate port with its own conversion
nuance (XM envelopes, MOD finetune, MTM/669 quirks); they build on the
conversion framework established by S3M (P1). Each format is independently
testable, and each already has a 1:1 reference in `IT_D_RM.INC`.

**Independent Test**: Open a known-good file of each format and confirm it loads,
plays, and displays correctly, with format-specific traits (MOD sample loops/
finetune, XM instruments/envelopes, MTM/669 patterns) converted as IT's own
`D_Load*` routine does.

**Acceptance Scenarios**:

1. **Given** a `.MOD` file, **When** opened, **Then** it converts to the IT song
   and plays, with its samples and pattern data represented as `D_LoadMOD` does.
2. **Given** an `.XM` file, **When** opened, **Then** its instruments,
   volume/panning envelopes and patterns convert to IT equivalents as
   `D_LoadXM` does, and play.
3. **Given** an `.MTM` or `.669` file, **When** opened, **Then** it converts and
   plays per `D_LoadMTM` / `D_Load669`.
4. **Given** any supported format, **When** the file is selected, **Then** the
   format is detected automatically (by signature/extension) and routed to the
   right loader, without the user specifying it.

---

### User Story 3 - Convert-and-save to .IT (Priority: P3)

A user who imported a non-IT module saves it as a native `.IT` file, effectively
converting the module permanently to IT format.

**Why this priority**: This turns import into a conversion tool, but it depends on
the Save module feature (#004) and on import (P1/P2) both existing. It is a
convenience capstone rather than the core of this feature.

**Independent Test**: Import an `.S3M`, save it as `.IT` (via #004), reload the
saved `.IT`, and confirm it plays equivalently to the imported module.

**Acceptance Scenarios**:

1. **Given** an imported non-IT module, **When** the user saves it, **Then** a
   valid `.IT` file is written (via the Save feature).
2. **Given** the saved `.IT`, **When** reloaded, **Then** it plays equivalently to
   the originally imported module.

---

### Edge Cases

- Format edge cases that have no exact IT equivalent (e.g. S3M/MOD effect
  semantics, MOD finetune, panning conventions) — mapped to the closest IT
  behaviour, with deviations documented (consistent with the README fidelity
  notes discipline).
- Unsupported or corrupt file / unknown signature — clear error, no crash,
  current song untouched (unless the user chose to replace it).
- Files whose extension and signature disagree — detection prefers the signature.
- Very old/edge MOD variants (channel counts, magic strings) — recognised where
  feasible; unrecognised variants reported clearly.
- Samples needing format conversion (bit depth, signedness, loop type) —
  converted to the engine's expected representation, with interpolator padding.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The loader MUST accept `.S3M` files and convert them into the IT
  in-memory song (orders, patterns, samples, instruments, channel/pan setup) so
  they play through the engine and are editable, ported from `D_LoadS3M`.
- **FR-002**: The loader MUST support the full set of formats IT itself imported
  natively — `.S3M`, `.XM`, `.MOD`, `.MTM`, `.669` (plus the existing `.IT`) —
  each ported from its `D_Load*` routine in `IT_D_RM.INC`. All five non-IT
  formats are committed scope for this feature (S3M is the P1 starting point for
  build order, but the feature is not considered complete until all five load).
- **FR-003**: The format MUST be auto-detected by signature (falling back to
  extension) and routed to the correct loader, mirroring IT's
  `D_LoadFile*Module` dispatch, without the user manually choosing a format.
- **FR-004**: Format-specific constructs (effects, finetune, panning, loop types,
  envelopes) MUST be converted as IT's own `D_Load*` routine converts them
  (matching IT behaviour, not a newly-invented mapping); any unavoidable
  deviation from the original loader MUST be documented in the README fidelity
  notes.
- **FR-005**: Imported modules MUST display correctly across the editor screens
  (patterns, samples/instruments, song variables) using the standard IT data
  model.
- **FR-006**: Imported samples MUST be converted to the engine's expected
  representation (bit depth/signedness, loop, interpolator padding).
- **FR-007**: The native `.IT` load path MUST remain unchanged and continue to
  load `.IT` files exactly as today.
- **FR-008**: A user MUST be able to save an imported module as `.IT` (via the
  Save feature #004), producing a valid file that reloads equivalently.
- **FR-009**: Unsupported/corrupt files MUST be reported clearly without crashing
  and without corrupting the current song unless replacement was chosen.
- **FR-010**: The audio-determinism regression for native `.IT` modules
  (`tests/test_pattern.c`) MUST remain `IDENTICAL` for all four testdata modules;
  conversion code MUST NOT affect the `.IT` path.

### Key Entities

- **Source module format**: A non-IT tracker format (`.S3M`, `.MOD`, `.XM`, …)
  with its own header, pattern, sample and effect conventions.
- **Format detector**: The logic that identifies a file's format by
  signature/extension and routes it to the right importer.
- **Conversion mapping**: The rules translating a source format's orders,
  patterns, effects, samples and instruments into the IT in-memory song.
- **IT in-memory song**: The single canonical engine song model all formats are
  converted into (unchanged for native `.IT`).

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A user can open a known-good `.S3M` and have it play and display
  correctly in the editor with no manual format selection.
- **SC-002**: All five native non-IT formats (`.S3M`, `.XM`, `.MOD`, `.MTM`,
  `.669`) load, play, and display correctly.
- **SC-003**: Each converted module plays/displays equivalently to how the
  original IT's `D_Load*` routine converts it, with any deliberate deviations
  from the original loader documented.
- **SC-004**: Native `.IT` loading is unaffected — the audio-determinism
  regression remains `IDENTICAL` for all four testdata modules.
- **SC-005**: Unsupported/corrupt inputs are rejected gracefully (clear message,
  no crash, current song safe) in 100% of tested bad inputs.

## Assumptions

- The conversion target is always the existing IT in-memory song model; importers
  are added alongside the current `.IT` loader (`it_load.c`) without changing the
  `.IT` path — exactly as the original kept all `D_Load*` routines converging on
  `D_PreLoadModule`/`D_PostLoadModule`.
- The committed format set is the one IT itself supported natively: S3M, XM, MOD,
  MTM, 669 (plus existing IT). S3M is the priority-1 starting point (explicitly
  requested and the closest sibling); the others follow. No new format beyond
  IT's native set is in scope.
- The fidelity bar is **matching IT's own conversion** (the `D_Load*` routines in
  `IT_D_RM.INC`), not bit-exact reproduction of the source tracker's playback —
  IT's importers already approximated. Deviations from IT's loader behaviour are
  documented per the project's fidelity discipline.
- Saving converted modules relies on the Save module feature (#004); if that is
  not yet present, import still delivers play/edit value for the session.
- This feature provides whole-module import; importing individual samples/
  instruments from such files as sources is covered by feature #006, which can
  build on these importers.
