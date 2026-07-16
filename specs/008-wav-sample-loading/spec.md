# Feature Specification: Standalone WAV Sample Loading

**Feature Branch**: `008-wav-sample-loading`

**Created**: 2026-07-05

**Status**: Draft

**Input**: User description: "Standalone WAV sample loading in the sample library
requester (F3 Enter). Port IT 2.17's standalone WAV file support from the original
disk/library code into the ittrack port: add .WAV to the requester's accepted-extension
filter, scan RIFF WAVE files into library records (format codes 5 = 8-bit WAV,
7 = 16-bit WAV as in the original loader table), and load/rip the selected WAV through
the existing sample-data conversion paths (unsigned 8-bit, signed 16-bit PCM mono).
Sample rate from the WAV fmt chunk becomes C5Speed. Match original 2.17 behaviour;
the stereo prompt stays out of scope as before, mono only — take the left channel
per the original's Left choice. AIFF and TXWave remain out of scope. This closes the
'WAV/AIFF/TXWave standalone sample loading' leftover from feature 006 (samples half,
WAV only)."

**Source of truth**: The original WAV identification and record synthesis in
`impulse-tracker-jthlim/IT_D_INF.INC` (`D_GetSampleInfo8` — "WAV Identification",
format codes 5/7) and the WAV branches of `D_LoadSampleData` in `IT_DISK.ASM`
(16-bit / unsigned conversion flags, the stereo channel-select data pass).
Behaviour MUST match IT's own, with the single documented deviation below
(no stereo prompt — left channel taken silently).

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Load a mono WAV file as a sample (Priority: P1)

A user on the sample list (F3) presses Enter on a slot, navigates the load
requester to a directory of `.WAV` files, sees them listed (today the pane shows
nothing), selects one and loads it into the chosen sample slot with a sensible
name, correct bit depth, and the file's sample rate as the playback reference
(C5 speed) — exactly the workflow the original IT supported.

**Why this priority**: This is the entire point of the feature — the user hit
this as a dead end (WAV folders appear empty). Plain mono 8/16-bit PCM WAV is
the overwhelmingly common case for sample libraries.

**Independent Test**: With a song loaded, press Enter on F3, browse to a folder
containing mono 8-bit and 16-bit PCM WAVs, confirm they are listed, load one of
each into slots, and confirm name/length/bit depth/C5 speed are right and each
auditions correctly through the engine at the expected pitch.

**Acceptance Scenarios**:

1. **Given** the sample-load requester open on a directory containing `.WAV`
   files, **When** the file pane is drawn, **Then** the WAV files are listed
   alongside the already-supported source formats.
2. **Given** a highlighted WAV file, **When** the info line refreshes, **Then**
   it identifies the file as an 8-bit or 16-bit sample per the original's
   format naming.
3. **Given** a selected mono 16-bit PCM WAV, **When** the user loads it,
   **Then** the target slot receives the sample data (signed 16-bit), the
   filename-derived name, the correct frame count, and the WAV's sample rate
   as C5 speed, and the rest of the song is unchanged.
4. **Given** a selected mono 8-bit PCM WAV, **When** the user loads it,
   **Then** the data is converted from unsigned 8-bit exactly as the original's
   conversion path does, and auditions correctly.
5. **Given** the loaded sample, **When** auditioned via the piano keys or
   previewed from the requester before loading, **Then** it plays through the
   real engine at the pitch implied by the stored C5 speed.

---

### User Story 2 - Load a stereo WAV (left channel, documented deviation) (Priority: P2)

A user loads a stereo PCM WAV. The original IT showed a "Loading Stereo Sample"
Left/Right prompt and loaded the chosen single channel (mono result). This port
skips the prompt — consistent with feature 006's scope — and silently takes the
left channel, which is the original's "Left" choice.

**Why this priority**: Stereo WAVs are common in the wild, so they must not be
rejected or mis-loaded; but the mono path (P1) is the core workflow and stands
alone without this.

**Independent Test**: Load a stereo 16-bit WAV; confirm the result is a mono
sample containing only the left channel, with the frame count halved relative
to a naive interleaved read, and that it auditions correctly.

**Acceptance Scenarios**:

1. **Given** a stereo PCM WAV, **When** it appears in the requester and info
   line, **Then** its record reports the per-channel frame count (not the
   interleaved total).
2. **Given** a stereo PCM WAV, **When** the user loads it, **Then** the target
   slot receives a mono sample made of the left channel only, and the
   deviation (no Left/Right prompt) is recorded in the project's fidelity
   notes.

---

### Edge Cases

- File has a `.WAV` extension but is not a valid RIFF PCM WAVE (wrong magic,
  compressed/float format tag, or 12/24/32-bit depth) — listed by extension but
  identified as unknown, and refused cleanly on load, per the original's
  identification rules (only PCM with 8 or 16 bits per sample qualifies).
- The `data` chunk is not the first chunk after `fmt ` — the original scans a
  bounded number of chunks before giving up; the port must reproduce that
  bounded scan (including its quirks) rather than a modern lenient parser.
- Oversized WAV — the original caps the loadable length; the port matches the
  cap rather than loading unbounded data.
- Truncated file (data chunk shorter than declared) — no read past end of
  file; load fails or truncates without corrupting the song.
- Loading into an occupied slot — the existing feature-006 "Replace sample N?"
  confirmation applies unchanged.
- Load during playback — sample installation stays serialised against the
  audio thread (engine lock), as all feature-006 loads are.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The sample-load requester (F3 Enter) MUST list files with the
  `.WAV` extension in addition to the feature-006 source set.
- **FR-002**: WAV identification MUST follow the original's rules
  (`D_GetSampleInfo8`): the RIFF `WAVE`/`fmt ` signature and PCM format tag
  are required; only 8- and 16-bit PCM qualify; the `data` chunk is found by
  the original's bounded chunk scan. Files that fail identification are
  reported with the original's "unknown" handling, not loaded.
- **FR-003**: Identified WAVs MUST be assigned the original's library format
  codes — 5 for 8-bit, 7 for 16-bit — and the info line MUST render the
  corresponding `SampleFormatNames` entries.
- **FR-004**: The synthesized library record MUST match the original's: name
  taken from the filename, default and global volume 64, frame count derived
  from the data-chunk size (halved for 16-bit and again for stereo, capped at
  the original's length limit), and the WAV sample rate stored as the C5
  speed exactly as the original stores it.
- **FR-005**: Loading MUST route the sample data through the existing
  feature-006 conversion paths: 8-bit data converted from unsigned, 16-bit
  data taken as signed little-endian, and the in-memory sample finalised the
  same way as every other feature-006 source (signed data, conversion flags
  cleared).
- **FR-006**: Stereo WAVs MUST load as mono using the left channel only, with
  the per-channel frame count; the omitted Left/Right prompt MUST be recorded
  as a deviation in the README fidelity notes and handoff doc.
- **FR-007**: A note key pressed on a highlighted WAV record MUST preview it
  through the check-slot mechanism, like other feature-006 sample sources.
- **FR-008**: Loads MUST NOT disturb the current song beyond the chosen slot;
  occupied-slot replacement keeps the feature-006 confirmation; installation
  is serialised against the audio thread.
- **FR-009**: The audio-determinism regression MUST remain `IDENTICAL` for
  all four testdata modules, and the existing selftest library gates
  (`LIB OK`, `IMPORT OK`) MUST still pass.
- **FR-010**: AIFF and TXWave standalone files remain out of scope; their
  absence continues to be listed as a feature-006 leftover.

### Key Entities

- **WAV source file**: A standalone RIFF WAVE file containing 8- or 16-bit PCM
  audio, mono or stereo, identified by the original's signature rules.
- **WAV library record**: The requester's synthesized entry for a WAV file —
  format code 5 or 7, filename-derived name, frame count, sample rate — the
  WAV counterpart of the module-scan records from feature 006.
- **Target sample slot**: The current song's sample slot receiving the loaded
  and converted data, as in feature 006.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A folder of `.WAV` files that previously displayed as empty in
  the sample-load requester now lists every WAV, and any mono 8/16-bit PCM
  WAV in it loads successfully in a few keystrokes.
- **SC-002**: 100% of loaded WAVs audition at the pitch implied by their file
  sample rate, and 8-bit and 16-bit variants of the same source audio sound
  alike after loading.
- **SC-003**: Stereo WAVs load as left-channel mono with correct length;
  no WAV load ever rejects a file the original IT would have accepted (mono
  or stereo 8/16-bit PCM), and none corrupts the song on malformed input.
- **SC-004**: Identification parity with the original: non-PCM or non-8/16-bit
  `.WAV` files are shown as unknown and refused, matching the original's
  acceptance boundary.
- **SC-005**: Audio-determinism regression stays `IDENTICAL` for all four
  testdata modules, and the selftest gains a WAV block (scan + rip + verify)
  alongside the existing `LIB OK` checks.

## Assumptions

- The scope is the samples half of the feature-006 leftover only: standalone
  `.WAV` loading. AIFF and TXWave remain out; the WAV stereo Left/Right prompt
  remains out (left channel taken silently) — both stay documented leftovers/
  deviations.
- The original's identification quirks are in scope as-is (they are the
  behaviour being ported): no check of the leading `RIFF` magic, the bounded
  data-chunk scan with its 16-bit chunk-size arithmetic, the length cap, and
  the 16-bit sample-rate field in the synthesized record.
- WAV *saving* (F3 Alt-W) already exists from feature 006 and is untouched;
  a saved WAV re-loading correctly through the new path is a natural
  round-trip check.
- The requester, record list, preview, replace-confirmation, and engine-lock
  machinery from feature 006 are reused unchanged; this feature adds a
  scanner/loader for one more source format, not new UI.
- Test WAV files are generated alongside the existing `lib_test.*` sources by
  the import-test generator, so the selftest stays self-contained.
