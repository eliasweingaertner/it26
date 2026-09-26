# Feature Specification: Authentic Load Sample Screen

**Feature Branch**: `015-load-sample-screen`

**Created**: 2026-09-26

**Status**: Draft

**Input**: User description: "Port the original Impulse Tracker 'Load Sample' screen (and its sibling 'View Sample Library' screen) instead of reusing the F9 module-load requester. In IT you see everything about the highlighted sample file before loading it; in ittrack you see a format name and a byte size."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Browse samples on the original's screen (Priority: P1)

A musician presses Enter on a sample slot and gets the same Load Sample screen
Impulse Tracker 2.14 shows: one numbered list in which subdirectories appear as
"Directory" rows beside their real names and files appear by name, a drive box,
and the screen furniture (header, boxes, labels) in the original's positions and
colours. They move through directories and drives entirely within that list.

**Why this priority**: This is the visible disparity reported by the user and in
GitHub issue #2. Without the right screen, nothing else in this feature has a
place to appear. On its own it already restores the original's navigation model.

**Independent Test**: Open Load Sample in a folder with subdirectories and sample
files, and compare against IT 2.14 in DOSBox on the same folder: same list
composition, same ordering, same directory rows, same drive box, same layout.

**Acceptance Scenarios**:

1. **Given** a folder containing subdirectories and sample files, **When** the user opens Load Sample from the F3 sample list, **Then** a single numbered list shows each subdirectory as a "Directory" row with its real name alongside, followed by the files, in the original's order.
2. **Given** the list, **When** the user selects a directory row (including the parent entry) and confirms, **Then** the list is replaced by that directory's contents.
3. **Given** the drive box, **When** the user selects a different drive, **Then** the list shows that drive's current directory.
4. **Given** the screen is open, **When** the user presses Escape, **Then** they return to the sample list with nothing loaded.
5. **Given** the screen is open, **When** it is compared cell by cell with the original, **Then** boxes, labels, colours and positions match.

---

### User Story 2 - See the sample before loading it (Priority: P2)

With a sample file highlighted, the musician sees its details without loading
it: the same parameter box as the sample list (filename, speed, loop settings,
loop points, quality, length), default and global volume, vibrato settings, a
waveform preview, and the file's format, size, date and time.

**Why this priority**: This is the "incomplete data" the user noticed. It is what
makes browsing a sample library in IT practical. It depends on US1 for its screen.

**Independent Test**: Highlight known WAV and IT-sample files and check that every
shown value matches what the sample list shows after actually loading each file,
and matches IT 2.14 for the same file.

**Acceptance Scenarios**:

1. **Given** a highlighted sample file, **When** the highlight lands on it, **Then** the parameter box shows its speed, loop mode and points, sustain loop mode and points, quality and length, formatted exactly as in the sample list.
2. **Given** a highlighted sample file, **When** the highlight lands on it, **Then** its waveform is drawn in the preview box.
3. **Given** any highlighted entry, **When** the highlight lands on it, **Then** format, size, date and time are shown; for a directory the format reads "Directory".
4. **Given** a file that is not a readable sample, **When** it is highlighted, **Then** the preview shows the empty-sample state rather than stale values from the previous file.
5. **Given** a module file highlighted, **When** the user opens it, **Then** its samples are browsable on the same screen with the same preview for each.

---

### User Story 3 - Adjust settings before loading (Priority: P3)

The musician changes the loop points, speed, volumes or vibrato on the preview
before loading, and those changes are what the loaded sample gets.

**Why this priority**: The original makes these fields editable. It saves a round
trip through the sample list but nothing depends on it.

**Independent Test**: Edit the speed and loop begin of a highlighted file,
load it, and confirm the sample list shows the edited values.

**Acceptance Scenarios**:

1. **Given** a highlighted file, **When** the user edits a preview field and loads the file, **Then** the loaded sample has the edited value.
2. **Given** a preview field was edited, **When** the user moves the highlight away, **Then** they are asked whether to save the sample file with the change; declining offers to discard it, and declining that keeps the highlight where it was.
3. **Given** an edited loop point outside the sample, **When** it is entered, **Then** it is corrected the same way the sample list corrects loop points.

---

### Edge Cases

- A directory with more entries than the list can show: the list scrolls and the numbering continues.
- Very long host filenames: shown truncated to the column width, and still loadable by their full name.
- An unreadable or damaged sample file: the preview shows the empty-sample state, loading it reports an error, and the target slot is unchanged.
- A drive with no medium or no permission: reported, and the list stays on the previous location.
- Loading in instrument mode: the existing "Create host instrument?" prompt appears exactly as today, defaulting to OK for an empty slot and No when replacing. No overwrite prompt appears.
- A stereo sample: the existing left/right choice still applies.
- Platforms without drive letters: the drive box offers the filesystem roots that exist.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Opening Load Sample from the sample list MUST show the original's Load Sample screen instead of the module-load layout.
- **FR-002**: The screen's boxes, labels, colours, glyphs and positions MUST come from the original's screen definition, not from visual approximation.
- **FR-003**: The list MUST show subdirectories as "Directory" rows with their real names beside them, and files by name, in one numbered list in the original's order.
- **FR-004**: Users MUST be able to enter directories, go to the parent, and change drives from within the screen.
- **FR-005**: For the highlighted entry the screen MUST show format, size, date and time.
- **FR-006**: For a highlighted sample the screen MUST show the parameter box, default and global volume, vibrato speed, depth and rate, and a waveform preview, taken from the file without loading it into the song.
- **FR-007**: Parameter values MUST use the same formatting as the sample list: seven zero-padded digits, "On"/"Off" plus "Forwards"/"Ping Pong", and "No sample" when there is no sample.
- **FR-008**: Every sample format the tracker can already load MUST be previewable, including samples inside module files.
- **FR-009**: Preview fields MUST be editable, and edited values MUST be applied to the sample when it is loaded.
- **FR-010**: Moving off an edited sample entry MUST offer to save the change to the file (or save under a new name when the filename was edited), else offer to discard it; declining both keeps the cursor on the entry (research R8b).
- **FR-011**: Loading MUST keep today's behaviour: no overwrite prompt; in instrument mode the "Create host instrument?" prompt with its existing defaults; the stereo left/right choice.
- **FR-012**: The in-module sample library view MUST use the same screen, as the original's library view shares it, and the original's separate Sample Library screen (Ctrl-F3) MUST be available.
- **FR-017**: Files the tracker cannot identify MUST still be listed (marked unknown), as the original lists every file.
- **FR-018**: Deleting the highlighted file from disk MUST be available behind a confirmation, as in the original.
- **FR-013**: A capture aid MUST let the screen be dumped or rendered to an image non-interactively, like the other screens.
- **FR-014**: The self-test MUST cover list composition with directories, preview values for a known WAV and a known IT sample, and an edited value surviving the load.
- **FR-015**: All existing gates MUST stay green: the determinism regression over the four reference modules, the round-trip check, and every existing self-test block.
- **FR-016**: The module-load screen (F9) MUST be unchanged.

### Key Entities

- **Browser entry**: One list row. Either a directory (with its real name) or a file, with its format, size, date and time.
- **Preview sample**: The header and waveform of the highlighted sample, held apart from the song and discarded or edited without affecting it until loaded.
- **Target slot**: The sample slot the user came from, which receives the loaded sample.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: For a test folder with at least five subdirectories and five sample files, the list shows the same rows in the same order as IT 2.14 in DOSBox.
- **SC-002**: For 100% of the formats the tracker can load, the preview values match the values the sample list shows after loading the same file.
- **SC-003**: A user can find out a sample's length, loop points and bit depth without loading it, in zero extra steps beyond highlighting it.
- **SC-004**: The GitHub issue #2 comparison — empty and loaded sample boxes side by side with IT — shows no remaining field that differs.
- **SC-005**: The reference-module determinism and round-trip results are identical to before the change.
- **SC-006**: Every existing self-test block passes, and the new block passes on both verification hosts.

## Assumptions

- The screen definition and its draw routines in the released source are complete enough to reproduce the layout exactly; where a detail is only visible in the DOSBox reference, that reference is authoritative and the source of the detail is recorded.
- Edited preview values being applied on load is the original's behaviour, confirmed in research: edits are written into the listing entry itself, which is what the load reads. (The spec's first draft said edits were discarded on moving the highlight; research showed the original keeps them on the entry.)
- Host filenames longer than the original's 8.3 names are truncated for display only.
- On platforms without drive letters, the drive box lists filesystem roots; this is a platform adaptation, not a behaviour change.
- The existing sample readers and the existing in-module library code supply all preview data; no new file format is added.
- The instrument-load screen is out of scope, even though it has a similar structure; it can follow in a later feature once this screen exists.
- MIDI and the original's network code paths are out of scope.
