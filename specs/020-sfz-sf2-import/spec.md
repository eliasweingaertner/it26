# Feature Specification: SFZ and SoundFont 2 Instrument Import

**Feature Branch**: master (no feature branch; local commits, not pushed until released)

**Created**: 2026-11-13

**Status**: Implemented (local, 2026-11-13)

**Input**: User description: "The next thing I'd like to build is additional import of sfz and sf2 instruments." Follow-up: "Please continue the design and the implementation autonomously. No pushes, local commits are fine." Real material: `D:\CommonHome\sound\sfz` (1387 .sfz files, samples as FLAC / WAV / AIFF) and `D:\CommonHome\sound\sf2` (1189 .sf2 files, 2.4 GB).

## Context

Impulse Tracker loads instruments from .ITI and .XI files and from
instruments inside IT and XM modules (F4 → Enter, the Load Instrument
screen). Today's sampled-instrument libraries ship as **SFZ** (a text file
describing regions that point at WAV / FLAC / AIFF files) or as
**SoundFont 2** (.sf2, one binary file holding samples and many presets).
it26 cannot read either, so this material has to be converted with other
tools first.

This feature lets the Load Instrument screen read both formats directly
and turn an SFZ instrument or an SF2 preset into an ordinary IT
instrument plus IT samples. It is an it26 extension ("classic IT with a
twist"): the result is a normal IT instrument that classic IT can play;
the engine is unchanged.

IT instruments are simpler than SFZ/SF2 instruments: one sample per key,
no velocity layers, no round robins, one volume envelope per instrument,
at most 99 samples in a song. The conversion therefore picks **one sound
per key** and approximates the rest; the rules are fixed and documented
so the result is predictable.

| Source | Shown on the Load Instrument screen as | Enter does |
|---|---|---|
| `.sfz` file | an instrument row (name = file name) | converts it into the current instrument |
| `.sf2` file | a library row, like an IT/XM module | opens it in place and lists its presets |
| preset inside an `.sf2` | an instrument row "bank:program name" | converts it into the current instrument |

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Load an SFZ instrument (Priority: P1)

A musician has an SFZ piano (FLAC samples, several velocity layers, round
robins). In F4 they press Enter, browse to the folder, see the .sfz file
listed with its name and sample count, press Enter on it and get an IT
instrument that plays the piano across the keyboard at the right pitches.

**Why this priority**: SFZ is the most common free sample-library format
today; the user's collection holds 1387 of them.

**Independent Test**: a generated SFZ with three regions (different keys,
roots, a loop, a velocity split, a round-robin pair) loads; each key plays
the expected sample at the expected pitch; loops and volume match.

**Acceptance Scenarios**:

1. **Given** a folder with .sfz files, **When** the Load Instrument screen lists it, **Then** every .sfz file appears as an instrument row with its name (file name without extension) and the number of samples it will need.
2. **Given** an SFZ whose regions use WAV (8/16/24/32-bit, float), AIFF or FLAC samples, **When** it is loaded, **Then** each key plays the sound of the region chosen for that key at the pitch the SFZ defines (root key, transpose, tune).
3. **Given** regions split by velocity, **Then** the loudest layer (the one covering the highest velocity) is used; **given** round robins or random alternatives, **then** the first one is used; **given** stacked layers on the same key and velocity, **then** the first region in file order is used.
4. **Given** regions conditioned on controllers or key switches, **Then** they are judged with the controllers at their default values and the default key switch (or, if none is set, the lowest key switch used), so a typical multi-articulation instrument loads its default articulation.
5. **Given** release-trigger regions, **Then** they are ignored.
6. **Given** a region with a loop (from the SFZ or the sample file), **Then** the IT sample loops there; a loop marked "sustain" becomes a sustain loop.
7. **Given** `#include`, `#define`, `default_path` and the `<global>`/`<master>`/`<group>`/`<region>` hierarchy, **Then** the regions get the settings an SFZ player would give them.
8. **Given** a region whose sample is missing or in an unsupported format (e.g. Ogg Vorbis), **Then** that region is skipped and the status line says how many were skipped; the rest of the instrument loads.

---

### User Story 2 - Load a SoundFont 2 preset (Priority: P1)

A musician presses Enter on an .sf2 file in the Load Instrument screen;
the screen shows the presets inside (like the instruments of a module),
each as "bank:program name". Enter on a preset loads it into the current
instrument.

**Why this priority**: SF2 is the classic General-MIDI / tracker-era
instrument format; the user's collection holds 1189 of them.

**Independent Test**: a generated SF2 with two presets (one mono with a
loop and a key split, one using a linked stereo sample pair) lists both
presets and loads each with the expected keys, pitches, loops and stereo.

**Acceptance Scenarios**:

1. **Given** an .sf2 file, **When** the list shows it, **Then** it is a library row; Enter opens it in place behind a "." row that leads back out, as modules do.
2. **Given** an opened .sf2, **Then** each preset is a row with "bank:program name" and the number of samples it needs, in bank/program order.
3. **Given** a preset, **When** it is loaded, **Then** keys, root keys, tuning (coarse, fine, pitch correction), loops, volume (attenuation) and pan follow the preset and instrument zones, including global zones and the layering of preset-level settings over instrument-level ones.
4. **Given** a preset using a linked left/right sample pair, **Then** the IT sample is stereo.
5. **Given** a preset whose zones overlap in velocity, **Then** the zone covering the highest velocity is used, as for SFZ.

---

### User Story 3 - Big instruments still fit (Priority: P2)

A sampled grand piano has a different sample for every key (88 or more
samples) — more than the free sample slots in the song.

**Why this priority**: Large libraries are the norm; refusing them would
make the feature useless for exactly the material people want.

**Acceptance Scenarios**:

1. **Given** an instrument needing more samples than are free, **When** it is loaded, **Then** it is reduced to fit: samples spread evenly over the keyboard are kept and every key plays the nearest kept sample, correctly pitched; the status line says the instrument was reduced (from how many to how many).
2. **Given** no free sample slot at all, **Then** the load is refused with IT's "Out of sample space" message.
3. **Given** samples already used only by the instrument being replaced, **Then** their slots count as free (as for .ITI/.XI loads).

---

### User Story 4 - Sound shape carried over (Priority: P2)

The converted instrument keeps the character of the original: its
attack/decay/release, its brightness and its stereo position.

**Acceptance Scenarios**:

1. **Given** an amplitude envelope (delay, attack, hold, decay, sustain, release), **Then** the IT instrument's volume envelope approximates it at IT's default tempo (125 BPM, 50 ticks per second), with a sustain point so the release plays at note-off.
2. **Given** a low-pass filter with cutoff and resonance, **Then** the instrument's filter cutoff/resonance are set to the nearest IT values; a cutoff above IT's range leaves the filter off.
3. **Given** per-region pan and volume, **Then** each IT sample's default pan and global volume carry them.
4. **Given** a sound with no envelope (organ-like), **Then** the volume envelope stays off and the note stops quickly at note-off.

---

### User Story 5 - Discoverable and documented (Priority: P3)

F1 on the Load Instrument screen and the README describe the new sources
and the conversion rules (one sound per key, which layer is chosen,
what is approximated, what is not supported).

### Edge Cases

- SFZ files with Windows line endings, comments (`//`, `/* */`), sample paths with spaces, backslashes, or upper/lower-case differences from the files on disk (case-insensitive fallback on case-sensitive file systems).
- Note names in SFZ (`c4`, `f#3`, `eb2`) and numbers; `key=` setting low key, high key and root key at once.
- Regions outside IT's note range (MIDI 120..127) are dropped; MIDI key k maps to IT note k (MIDI 60 = C-5).
- A region with `offset`/`end` uses that part of the sample; a loop lying outside it is dropped.
- SF2 with 24-bit data (`sm24`): the upper 16 bits are used. SF3 (compressed SoundFont, Ogg samples): presets are listed but loading reports "unsupported (SF3)".
- An SF2 or SFZ with no usable region: "Unable to load instrument." and nothing changes.
- Very large SF2 files (hundreds of MB): opening the library and loading a preset read only what is needed, not the whole file.
- The same sample used by many keys with the same root key and settings becomes one IT sample, not many.
- Stereo SFZ samples load as stereo IT samples.
- Pitch key-tracking other than 100 % and other unsupported features (filters other than low-pass, LFOs, modulators, effects, velocity curves) are ignored.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The Load Instrument screen and the system Open Instrument dialog MUST recognise `.sfz` files as instruments and `.sf2` files as instrument libraries (SF2 recognised by content, SFZ by extension).
- **FR-002**: Entering an `.sf2` MUST list its presets as instrument rows "bank:program name" with the number of samples each needs, behind the usual exit row.
- **FR-003**: Loading an SFZ file or SF2 preset MUST replace the current instrument and place the samples into free sample slots, releasing samples used only by the replaced instrument, as .ITI/.XI loading does, and enable instrument mode on request as today.
- **FR-004**: For each IT note 0..119 the converter MUST choose at most one region/zone: matching the key; covering the highest velocity among matches; first round-robin / random alternative; default controller values and default key switch; release-trigger regions excluded; first in file order among the rest.
- **FR-005**: Every chosen region MUST play at its source pitch: root key, transpose, coarse/fine tune and pitch correction baked into the IT sample's playback rate, with the note table mapping each key to itself.
- **FR-006**: Regions sharing the same sample data, range and pitch/loop/volume/pan settings MUST share one IT sample.
- **FR-007**: When more IT samples are needed than slots are free, the converter MUST keep an evenly spread subset that fits and map every key to the nearest kept sample (pitch stays correct) and report the reduction.
- **FR-008**: Sample data MUST be read from WAV (8/16/24/32-bit integer, 32/64-bit float), AIFF and FLAC for SFZ, and from the SF2's own sample block; data is stored as 16-bit IT samples (8-bit sources may stay 8-bit); stereo sources become stereo IT samples; SF2 linked left/right pairs become one stereo sample.
- **FR-009**: Loops MUST be taken from SFZ opcodes, else from the sample file's loop chunk, and from SF2 sample modes and loop points; continuous loops become IT loops, loops that run only while the key is held become IT sustain loops.
- **FR-010**: Volume (SFZ volume, SF2 attenuation) MUST set each IT sample's global volume; pan MUST set the sample's default pan.
- **FR-011**: The amplitude envelope of a representative region (the one at C-5, else the first used) MUST become the instrument's volume envelope (on, with sustain point and release) unless it is flat; low-pass cutoff/resonance MUST become the instrument filter when in IT's range.
- **FR-012**: The instrument name MUST be the SFZ file name without extension or the SF2 preset name.
- **FR-013**: Missing or unsupported sample files MUST skip their regions and be counted in the status message; a file that yields no usable region MUST leave the song unchanged and report "Unable to load instrument."
- **FR-014**: Opening an SF2 library and loading a preset MUST NOT require reading the whole file into memory.
- **FR-015**: The engine (playback, mixer, tables) MUST NOT change; songs without such instruments MUST render bit-identically.
- **FR-016**: F1 help and the README MUST describe the new sources and the conversion rules.

### Key Entities

- **SFZ region**: a sample file plus the opcodes that apply to it after inheritance (key range, velocity range, root key, tuning, loop, volume, pan, envelope, filter, trigger/round-robin/switch conditions).
- **SF2 preset / zone**: a preset's zones, each pointing at an instrument whose zones point at samples; generators of both levels combine.
- **Converted instrument**: the IT instrument (note table, envelope, filter, name) and its IT samples (data, rate, loops, volume, pan, stereo).

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Of the user's 1387 SFZ files, at least 95 % that reference only WAV/AIFF/FLAC samples present on disk load into an empty song without error.
- **SC-002**: Of the user's 1189 SF2 files, at least 98 % open and list their presets, and at least 98 % of presets in uncompressed SF2 files load into an empty song without error.
- **SC-003**: For the generated test instruments, every key plays the expected sample at a pitch within 1 cent of the expected one, and loops, volume, pan and stereo match the source exactly.
- **SC-004**: Loading a typical preset or SFZ (≤ 50 MB of sample data) takes under 3 seconds; opening an SF2 library of any size lists its presets in under 1 second.
- **SC-005**: The four reference modules still render bit-identically and every existing selftest block passes.

## Assumptions

- IT plays a note at full volume by default, so the loudest velocity layer is the natural single choice.
- MIDI key 60 corresponds to IT's C-5 (the convention of OpenMPT and Schism Tracker).
- Envelope times are converted at 50 ticks/second (IT's default 125 BPM); songs at other tempos get proportionally different envelope timing — an inherent limit of IT envelopes.
- Ogg Vorbis (SFZ) and SF3 are out of scope: none of the user's SFZ files use Ogg, and no Vorbis decoder is vendored.
- Only the F4 Load Instrument path is in scope; the F3 Load Sample screen does not list SF2 samples in this feature.
- No velocity-layer splitting into several instruments in this feature (possible follow-up).
