# Feature Specification: Save Module (F10) & Message Editor

**Feature Branch**: `004-save-module`

**Created**: 2026-06-16

**Status**: Draft

**Input**: User description: "Message editor + save module (F10). F10 needs the IT_DISK.ASM save path ported (the engine only loads today). The menu entries exist and flash 'not ported yet'. Roadmap item #4 in docs/HANDOFF.md §6."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Save a module to a .IT file (Priority: P1)

A user who has loaded and edited a module presses F10 (or chooses Save from the
File menu) and writes the song back out as a valid `.IT` file that Impulse
Tracker itself, and this port's own loader, can read back identically. Today the
editor can only load; the Save entries flash "not ported yet."

**Why this priority**: Saving is the single missing capability that turns the
editor from a viewer/auditioning tool into something that can preserve work. It
is the foundation; the save requester (P2) and message editor (P3) refine it.

**Independent Test**: Load one of the testdata modules, save it to a new `.IT`
file, reload the saved file, and confirm the song is structurally identical and
renders byte-identical audio to the original (round-trip through save→load).

**Acceptance Scenarios**:

1. **Given** a loaded module, **When** the user invokes Save (F10), **Then** a
   valid `.IT` file is written containing the song header, orders, instruments,
   sample headers, sample data, patterns and MIDI macro data.
2. **Given** a saved file, **When** it is reloaded by this port, **Then** the
   in-memory song matches the pre-save song and renders byte-identical audio.
3. **Given** a saved file, **When** opened in a reference IT-compatible player,
   **Then** it loads and plays correctly (interoperable format).
4. **Given** patterns edited in the session, **When** the module is saved,
   **Then** the saved patterns reflect the edits exactly.

---

### User Story 2 - Save requester and overwrite handling (Priority: P2)

A user saving a module chooses the filename and directory through the file
requester (consistent with the F9 load requester) and is protected from
accidentally clobbering an existing file without confirmation.

**Why this priority**: A usable save flow needs a destination chooser and
overwrite safety; it builds directly on the save path (P1) and reuses the
existing requester widgetry, so it is a clear second step.

**Independent Test**: Invoke Save, navigate directories, type a filename, save;
then save again to the same name and confirm an overwrite confirmation appears;
confirm cancelling leaves the existing file untouched.

**Acceptance Scenarios**:

1. **Given** Save is invoked, **When** the requester appears, **Then** it
   presents file/dir/drive navigation and an editable filename field consistent
   with the F9 load requester.
2. **Given** a filename that already exists, **When** the user confirms save,
   **Then** an overwrite confirmation is shown before writing.
3. **Given** the user cancels at the overwrite prompt, **When** dismissed,
   **Then** no file is written and the existing file is unchanged.

---

### User Story 3 - Song message editor (Priority: P3)

A user edits the module's embedded song message (the multi-line text IT stores
in a song) and that text is written into the saved `.IT` file.

**Why this priority**: The message editor is a self-contained editing surface
that pairs with saving (the message is only meaningfully persisted once save
exists). It is the lowest priority because a module saves correctly without the
user editing the message.

**Independent Test**: Open the message editor, type and edit multi-line text,
save the module, reload it, and confirm the message round-trips intact.

**Acceptance Scenarios**:

1. **Given** a loaded module, **When** the user opens the message editor, **Then**
   the existing song message is shown and is editable as multi-line text.
2. **Given** edited message text, **When** the module is saved and reloaded,
   **Then** the message is preserved exactly.
3. **Given** a module with no message, **When** the editor is opened, **Then** an
   empty editable message is presented and can be added.

---

### Edge Cases

- Saving compressed vs. uncompressed sample data — output is a valid `.IT` the
  loader reads back identically (compression choice must round-trip).
- Module with old-format / best-effort-converted instruments — save produces a
  current-format file without data loss beyond what conversion already implied.
- Read-only destination / no write permission / full disk — fail with a clear
  message, leaving any existing file intact.
- Very large modules (many patterns/large samples) — saved completely and
  correctly.
- Empty/new song (if creatable) — saves a minimal valid `.IT`.
- Filename without `.it` extension — handled per IT convention (extension
  applied) without surprising the user.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The editor MUST write the in-memory song to a valid, IT-compatible
  `.IT` file covering header, orders, instruments, sample headers, sample data,
  patterns and MIDI macro data — porting the `IT_DISK.ASM` save path.
- **FR-002**: A save→load round-trip MUST reproduce the song such that it renders
  byte-identical audio to the original (within the existing determinism
  guarantees) and is structurally equivalent.
- **FR-003**: Saved files MUST be readable by reference IT-compatible software
  (interoperable, standard `.IT` format).
- **FR-004**: Users MUST choose the destination via a file requester consistent
  with the existing F9 load requester (dirs/drives/editable filename).
- **FR-005**: The system MUST warn before overwriting an existing file and MUST
  not write if the user cancels.
- **FR-006**: The system MUST provide a song message editor whose text is
  persisted into the saved `.IT` file and round-trips on reload.
- **FR-007**: Save failures (permission, disk full, invalid path) MUST report a
  clear message and leave any existing target file unchanged.
- **FR-008**: Saving MUST serialise against the audio thread (engine lock) so a
  consistent snapshot is written during live playback.
- **FR-009**: The Save / File-menu entries that currently flash "not ported yet"
  MUST become functional.
- **FR-010**: The audio-determinism regression (`tests/test_pattern.c`) MUST
  remain `IDENTICAL` for all four testdata modules; saving MUST NOT alter the
  in-memory song or playback.

### Key Entities

- **Module file (.IT)**: The on-disk representation — header, orders,
  instruments, sample headers + (optionally compressed) sample data, packed
  patterns, MIDI macros, and the song message.
- **Song message**: Multi-line embedded text stored in the module.
- **Save requester**: The destination chooser (directory/drive navigation +
  editable filename), mirroring the load requester.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 100% of the four testdata modules can be loaded, saved, and
  reloaded with byte-identical rendered audio and equivalent structure.
- **SC-002**: Saved files load and play correctly in at least one independent
  IT-compatible player (interoperability confirmed).
- **SC-003**: Edits made in the session (patterns, instruments, song variables,
  message) are all present in the saved file.
- **SC-004**: Overwriting an existing file always prompts for confirmation; no
  file is ever written when the user cancels.
- **SC-005**: Audio-determinism regression remains `IDENTICAL` for all four
  testdata modules.

## Assumptions

- The save format target is IT's standard `.IT` (the format this port already
  loads), implemented from the `IT_DISK.ASM` save path against `ITTECH.TXT`.
- Sample-data compression on save is supported such that it round-trips through
  the existing 2.14/2.15 decompression loader; uncompressed output is acceptable
  if it round-trips identically.
- Save reuses the existing file-requester widgetry from the F9 load screen.
- Exporting to non-IT formats (S3M/MOD/etc.) is out of scope here (the import
  side is a separate feature); this feature is `.IT` save only.
- "Byte-identical audio after round-trip" is the fidelity bar, consistent with
  the existing `tests/test_pattern.c` harness; the save path should extend that
  harness to cover save→load.
