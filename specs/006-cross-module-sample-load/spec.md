# Feature Specification: Load Samples & Instruments from Other Modules

**Feature Branch**: `006-cross-module-sample-load`

**Created**: 2026-06-16

**Status**: Draft

**Input**: User description: "Loading samples from other modules — let the user browse an existing module/library file and pull individual samples (and instruments) into the current song, as IT's sample/instrument load requesters do. The original IT did this natively: `IT_D_RIS.INC` ('Read Instrument Sample') has a whole family of `Load*SamplesInModule` routines (MOD, S3M, FAR, MTM, ULT, 669, PTM, IT, XM), and `IT_D_RI.INC` ('Read Instrument') has `LoadITInModuleInstrument` / `LoadXMInModuleInstrument` for pulling a full instrument out of another module (plus `.ITI`/`.XI` instrument files and `.KRZ`/`.PAT` sample files). This port currently only loads a whole .IT into the global Song. Port IT's existing ripping so the user can pull a single sample or instrument from another file into the current song."

**Source of truth**: The original sample/instrument library loaders in
`impulse-tracker-jthlim/IT_D_RIS.INC` (`Load{MOD,S3M,FAR,MTM,ULT,669,PTM,IT,XM}
SamplesInModule`, plus `.KRZ`/`.PAT`) and `IT_D_RI.INC`
(`LoadITInModuleInstrument`, `LoadXMInModuleInstrument`, `LoadITInstrument`,
`LoadXIInstrument`). Behaviour MUST match IT's own.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Load a single sample from another module (Priority: P1)

A user on the sample list (F3) picks an empty (or selected) sample slot, opens a
load requester, browses to another module or sample file, previews/selects one
sample inside it, and loads just that sample into the current song — without
replacing the whole song. Today there is no way to do this; loading a file
replaces the entire global song.

**Why this priority**: Pulling individual samples from a library of existing
modules is the core of the request and the most common real workflow. It is the
foundational slice; instrument import and previewing build on the same browsing
machinery.

**Independent Test**: With a song loaded, open the sample-load requester from F3,
browse to a second `.IT` module, choose one of its samples, load it into a chosen
slot, and confirm: the current song's other content is untouched, the new sample
appears in the slot with its name/parameters, and it auditions correctly through
the engine.

**Acceptance Scenarios**:

1. **Given** a loaded song and a target sample slot, **When** the user loads a
   sample from another module file, **Then** that sample (data + header
   parameters + name) is placed in the target slot and the rest of the song is
   unchanged.
2. **Given** the imported sample, **When** auditioned via the piano keys, **Then**
   it plays correctly through the real engine.
3. **Given** an occupied target slot, **When** the user imports into it, **Then**
   the user is warned before the existing sample is replaced.

---

### User Story 2 - Load an instrument (with its samples) from another module (Priority: P2)

A user imports a full instrument from another module — bringing in the
instrument definition together with the sample(s) it references, remapped into
free slots of the current song.

**Why this priority**: Instrument import is the natural extension of sample
import and very useful, but it is more involved (it must also bring and remap the
referenced samples). It depends on the sample-import machinery from P1.

**Independent Test**: Import an instrument from a second module into the current
song and confirm the instrument and all samples it references are added (in free
slots, with references correctly remapped), and that auditioning the instrument
plays its samples correctly.

**Acceptance Scenarios**:

1. **Given** a target instrument slot, **When** the user imports an instrument
   from another module, **Then** the instrument and its referenced sample(s) are
   added and the instrument's sample references are remapped to the new slots.
2. **Given** insufficient free sample slots, **When** importing an instrument,
   **Then** the user is informed and no partial/corrupt import is left behind.
3. **Given** the imported instrument, **When** auditioned, **Then** it plays
   correctly through the engine using the imported samples.

---

### User Story 3 - Browse and preview a source module's contents (Priority: P3)

A user browsing a source module sees its list of samples/instruments (names) and
can preview/audition an item before importing, so they pick the right one.

**Why this priority**: Preview/browse improves the workflow and reduces wrong
imports, but import already works without it (P1/P2). It is a usability
refinement, hence lowest priority.

**Independent Test**: Open a source module in the requester, see its named
sample/instrument list, audition an item, then import it.

**Acceptance Scenarios**:

1. **Given** a selected source module, **When** the requester opens it, **Then**
   its samples/instruments are listed by name/index without loading the whole
   song.
2. **Given** a listed item, **When** the user previews it, **Then** it auditions
   without being imported.
3. **Given** a previewed item, **When** the user confirms, **Then** it is
   imported per User Story 1/2.

---

### Edge Cases

- Source file is the same module currently loaded — handled without corrupting
  the live song.
- Source sample is compressed (IT 2.14/2.15) — decompressed on import like the
  main loader does.
- Source uses a different/older format (if multi-format import #007 is present) —
  samples/instruments are converted on import; otherwise unsupported sources are
  reported clearly.
- Importing into a slot that is in use during playback — serialised against the
  audio thread so the mixer never reads a half-written sample.
- Name/encoding differences between source and target — names imported sensibly.
- Source file missing/corrupt/unreadable — clear error, current song untouched.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The editor MUST let the user load an individual sample from an
  external module/sample file into a chosen sample slot of the current song,
  without replacing the rest of the song.
- **FR-002**: An imported sample MUST carry its data, header parameters and name,
  be decompressed if compressed, and audition correctly through the engine,
  matching IT's `Load*SamplesInModule` behaviour.
- **FR-002a**: Source modules for sample ripping MUST cover the formats IT
  itself read samples from — IT, XM, S3M, MOD, MTM, 669, FAR, ULT, PTM — ported
  from the corresponding `Load*SamplesInModule` routines. Standalone sample files
  (`.KRZ` Kurzweil, `.PAT` Gravis) and instrument files (`.ITI`, `.XI`) are in
  scope as additional sources, per the original. The full IT-native source set is
  committed scope; the feature is complete only when all listed sources rip
  correctly (build order is a planning decision).
- **FR-003**: The editor MUST let the user import a full instrument from an
  external module, including the sample(s) it references, placing them in free
  slots and remapping the instrument's sample references accordingly — ported
  from `LoadITInModuleInstrument` / `LoadXMInModuleInstrument`.
- **FR-004**: Importing into an occupied slot MUST warn before replacing; an
  instrument import with insufficient free sample slots MUST be reported and MUST
  NOT leave a partial/corrupt import.
- **FR-005**: The requester MUST let the user browse to a source file and select
  the specific sample/instrument to import, consistent with the existing file
  requester UX, and SHOULD list the source's samples/instruments by name.
- **FR-006**: The user SHOULD be able to preview/audition a source item before
  importing.
- **FR-007**: Reading a source file MUST NOT disturb the currently loaded song
  except for the explicit import into the chosen slot(s).
- **FR-008**: Imports that mutate song data the mixer may read MUST serialise
  against the audio thread (engine lock).
- **FR-009**: The audio-determinism regression (`tests/test_pattern.c`) MUST
  remain `IDENTICAL` for all four testdata modules (no-import path unchanged).

### Key Entities

- **Source module/sample file**: An external file (`.IT`, and other formats if
  #007 is present, or a standalone sample file) read for the purpose of importing
  selected content, not to replace the current song.
- **Importable sample**: A sample within a source — data, parameters, name —
  destined for a target slot.
- **Importable instrument**: An instrument within a source plus the samples it
  references, requiring slot allocation and reference remapping on import.
- **Target slot**: The current song's sample/instrument slot receiving the
  imported content.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A user can import a single sample from another module into the
  current song in a few steps, with the rest of the song provably unchanged.
- **SC-002**: An imported sample/instrument auditions correctly through the engine
  in 100% of the supported source formats.
- **SC-003**: Instrument import brings all referenced samples and remaps
  references so the instrument plays exactly as in the source.
- **SC-004**: No import ever leaves the song in a partial/corrupt state; occupied
  slots are never overwritten without a warning.
- **SC-005**: Audio-determinism regression remains `IDENTICAL` for all four
  testdata modules.

## Assumptions

- Source parsing reads the chosen format into a temporary/secondary structure
  rather than the global `Song`, so the live song is not clobbered — this is how
  IT's `Load*SamplesInModule` / `Load*InModuleInstrument` routines worked
  (reading from a disk-data scratch area, not the live song).
- The supported source set is IT's own: IT, XM, S3M, MOD, MTM, 669, FAR, ULT,
  PTM modules for sample ripping; IT/XM modules for instrument ripping; plus
  `.ITI`/`.XI` instrument files and `.KRZ`/`.PAT` sample files. These overlap
  with — but are independent of — the whole-module importers in feature #007;
  this feature can share format-parsing code with #007 where the two read the
  same containers.
- This mirrors IT's own sample/instrument load requesters in behaviour and UX,
  reusing the file-requester widgetry from the F9 load screen.
- Persisting the resulting song to disk depends on the Save module feature
  (#004); within this feature imports persist for the session.
