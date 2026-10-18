# Feature Specification: Stereo Samples

**Feature Branch**: master (no feature branch; not pushed until released)

**Created**: 2026-10-18

**Status**: Draft

**Input**: User description: "Assuming we'd like to go real 2026 and support stereo samples / instruments" — load, play, edit, save and export true stereo samples, as an it26 extension ("classic IT with a twist"), while songs without stereo samples behave and sound exactly as before.

## Context

Impulse Tracker 2.14/2.17 has mono samples only. The IT file format
already reserves a "stereo" flag for samples (data stored as the left
channel followed by the right), and later trackers (OpenMPT, Schism
Tracker) read and write it — but IT's own mixer ignores the flag, and
loading a stereo WAV asks **"Loading Stereo Sample: Left / Right"** and
keeps one channel. Today it26 does the same.

Modern sample material is mostly stereo: drum loops, pads, field
recordings, renders from a DAW — and since feature 018, renders of the
song itself. Forcing them to one channel loses the stereo image the
material was made with. This feature makes stereo a first-class sample
type in it26, everywhere samples are used, without changing anything for
songs that only use mono samples.

| Area | Today (as IT 2.14) | With this feature |
|---|---|---|
| Loading a stereo WAV/AIFF | asks Left / Right, keeps one | asks **Stereo** / Left / Right, Stereo first |
| Stereo samples inside a module | not loaded as stereo | loaded and played as stereo |
| Playback | mono samples, panned | stereo samples keep their image; pan acts as balance |
| Sample editor (F3) | one waveform | both channels shown and edited |
| Saving | mono only | stereo kept in IT modules, ITS samples, ITI instruments |
| Renders into a sample (feature 018) | mixed to mono | stay stereo when the song is stereo |

## Clarifications

### Session 2026-10-18

- Q: How does panning apply to a stereo sample? → A: As **balance**: centre plays both channels as recorded; panning towards one side turns the other channel down (the same as OpenMPT and Schism Tracker). Pan envelopes, channel pan, default pan and pan slides all act through the same balance.
- Q: What does a module with stereo samples do in classic IT 2.14? → A: It opens; IT plays each stereo sample's left channel. That is graceful degradation, documented in the README.
- Q: Which choice does the stereo loading requester offer first? → A: **Stereo** (new, default), then IT's Left and Right with their original behaviour.
- Q: How are stereo samples saved when IT214/IT215 compression is selected? → A: **Uncompressed**, as OpenMPT does: three OpenMPT 1.32 saves wrote every stereo sample uncompressed while compressing the mono ones. Mono samples are compressed as chosen. Compressed stereo from other writers is still read (each channel's compressed stream in turn).
- Q: Reference file? → A: `testdata/openmpt-testpattern-stereo.it` (OpenMPT 1.32, 1.18 MB, sample mode): 16-bit stereo with a loop (left/right almost uncorrelated), 16-bit stereo without loop, 8-bit stereo with a ping-pong loop.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Load and play a stereo sample (Priority: P1)

A musician loads a stereo drum loop (WAV) into a sample slot. it26 asks
"Loading Stereo Sample: Stereo / Left / Right" with Stereo selected; Enter
keeps both channels. Played from the pattern, the loop sounds as wide as
it was recorded.

**Why this priority**: The core of the feature; everything else builds on
a sample being stereo and playing as such.

**Independent Test**: Load a stereo WAV whose left and right channels
differ (e.g. a tone only on the left, silence on the right), play it at
C-5 with centre pan: the output has the tone on the left channel only;
with Left chosen at load time the sample is mono, as in IT 2.14.

**Acceptance Scenarios**:

1. **Given** a stereo WAV or AIFF, **When** it is loaded into a slot (Load Sample screen, sample library, system file dialog, hot swap), **Then** the requester offers Stereo (default), Left, Right; Stereo keeps both channels, Left/Right keep one as IT does.
2. **Given** a stereo sample, **When** a note plays it, **Then** both channels are heard, each on its own side, through the same interpolation, resonant filter, volume ramping and click removal as a mono sample.
3. **Given** a stereo sample, **Then** loops, sustain loops, ping-pong loops, the sample offset command, note-off / note-cut / fade, new-note actions and duplicate checks behave exactly as for a mono sample of the same length.
4. **Given** a stereo sample panned with default pan, channel pan, pan commands, pan slides or the pan envelope, **Then** the pan acts as balance: centre = as recorded, full left = left channel only at full level, full right = right channel only.
5. **Given** surround (pan 100) on a stereo sample, **Then** it behaves as IT's surround does for the mono mix of the sample (documented).
6. **Given** a song with no stereo samples, **Then** playback is bit-identical to before this feature.

---

### User Story 2 - Keep stereo when saving and exchanging (Priority: P1)

A musician saves a song that contains stereo samples and opens it again
— the samples are still stereo. A friend opens the same module in
OpenMPT or Schism Tracker and hears stereo; someone with classic IT 2.14
can still open it and hears the left channel.

**Why this priority**: Without saving, stereo samples are lost on the
next load; interchange is what makes the feature usable outside it26.

**Independent Test**: Load `testdata/openmpt-testpattern-stereo.it`
(written by OpenMPT), save it as IT (also with IT215 selected), reload it
in it26 → identical stereo data; load it in OpenMPT/Schism → stereo.

**Acceptance Scenarios**:

1. **Given** a module containing stereo samples, **When** it is saved as IT (with or without compression selected — stereo samples are always written uncompressed, as OpenMPT does) and loaded again, **Then** every stereo sample is stereo with identical data.
2. **Given** an IT module written by OpenMPT or Schism Tracker with stereo samples (uncompressed or compressed), **When** it is loaded, **Then** those samples are stereo.
3. **Given** a stereo sample or an instrument using stereo samples, **When** it is saved as an ITS sample or ITI instrument and loaded again, **Then** it is stereo.
4. **Given** a module with stereo samples, **When** it is exported as S3M, **Then** stereo samples are mixed down to mono and a warning names the reason, like the existing S3M warnings.
5. **Given** a module with stereo samples, **When** it is opened in IT 2.14, **Then** it loads and plays (the left channel of each stereo sample).

---

### User Story 3 - Edit stereo samples in F3 (Priority: P2)

A musician works on a stereo sample in the sample list: sees both
channels, trims, reverses, amplifies or resamples it — and the stereo
image survives every operation. When a mono version is needed, the
sample can be converted.

**Why this priority**: Stereo samples have to survive the everyday sample
operations, or musicians lose the image the moment they edit.

**Independent Test**: A stereo sample with different content per
channel: reverse, amplify 50 %, resize to half length, cut before loop —
after each operation the left and right channels match the same operation
applied to each channel separately.

**Acceptance Scenarios**:

1. **Given** a stereo sample in F3, **Then** the waveform shows both channels and the sample info shows it is stereo.
2. **Given** a stereo sample, **When** any of IT's sample operations is used (reverse, invert, centralise, amplify, resize with and without interpolation, cut before loop, cut, signed/unsigned conversion, delete, copy, replace, swap, exchange), **Then** it applies to both channels consistently.
3. **Given** a stereo sample, **When** "convert to mono" is chosen, **Then** the user picks mix down, left or right, and the sample becomes mono.
4. **Given** a mono sample, **When** "convert to stereo" is chosen, **Then** it becomes a stereo sample with both channels equal.
5. **Given** a stereo sample, **When** it is exported as a WAV (feature 018 export, IT's save-as-WAV), **Then** the WAV is stereo.

---

### User Story 4 - Stereo renders into samples (Priority: P3)

A musician bounces a pattern of a stereo song into a sample (feature 018,
Ctrl-O): the new sample is stereo instead of mixed to mono.

**Acceptance Scenarios**:

1. **Given** a song in stereo mode, **When** a pattern is rendered into a sample, **Then** the sample is stereo; in mono mode it stays mono.
2. **Given** the 1 MB cap for imported renders, **Then** it counts the stereo data (half the time of a mono render).

---

### User Story 5 - Discoverable (Priority: P3)

F1 lists the new stereo operations under "it26 additions"; the README
describes stereo samples as an extension, including what classic IT does
with such modules.

### Edge Cases

- A stereo WAV with 8-bit data, 16-bit data, odd byte counts, or a declared length longer than the file: loads like the mono equivalents do today (truncated to what is there).
- A stereo sample longer than the per-slot size limit: cut to the limit (counting both channels).
- Converting a looped stereo sample to mono keeps its loop points; converting to stereo too.
- Copying a stereo sample over a mono one (and back) in replace/swap/exchange: the target takes the source's channel count.
- Sample memory: a stereo sample uses twice the memory of a mono one of the same length; "Free memory" displays account for it.
- Instrument mode with an instrument mapping notes to both mono and stereo samples: each note plays its sample as it is.
- Surround on a stereo sample: documented behaviour (US1 #5).
- The S3M writer's warnings list stereo samples once, not once per sample.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Samples MUST be either mono or stereo; stereo samples MUST keep two independent channels of equal length.
- **FR-002**: Loading a stereo WAV/AIFF MUST offer Stereo (default), Left, Right; Left/Right MUST behave as IT 2.14.
- **FR-003**: IT modules, ITS samples and ITI instruments MUST load and save stereo samples (flag set, left channel data followed by right), compatible with OpenMPT and Schism Tracker. Stereo samples MUST be written uncompressed whatever compression is selected (as OpenMPT); compressed stereo from other writers MUST be read.
- **FR-004**: Playback MUST mix both channels of a stereo sample with the same interpolation, filter, ramping, looping and channel logic as a mono sample; pan of any kind MUST act as balance.
- **FR-005**: Songs without stereo samples MUST play bit-identically to the previous version (determinism gate).
- **FR-006**: Every IT sample operation in F3 MUST handle stereo samples; "convert to mono" (mix / left / right) and "convert to stereo" MUST be available on keys IT 2.14 leaves unused.
- **FR-007**: The F3 waveform MUST show both channels of a stereo sample; the sample info MUST show stereo.
- **FR-008**: S3M export MUST mix stereo samples down to mono with a warning.
- **FR-009**: Sample export to WAV (IT's and feature 018's) MUST write stereo samples as stereo WAVs.
- **FR-010**: Renders into a sample (feature 018) MUST produce stereo samples for songs in stereo mode.
- **FR-011**: F1 and the README MUST document stereo samples, their keys, and how IT 2.14 treats such modules.

### Key Entities

- **Sample**: gains a channel count (1 or 2); length, loop points and sample offsets count frames (one value per channel).
- **Instrument**: unchanged; maps notes to samples, mono or stereo.
- **Module file**: the existing IT sample flag for stereo, left then right data.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The four reference modules render bit-identically before and after the feature.
- **SC-002**: The OpenMPT reference file loads with its three stereo samples as stereo, saves (with and without compression selected) and reloads in it26 with identical data, and the saved file plays in stereo in OpenMPT and Schism Tracker.
- **SC-003**: For each F3 operation, the result on a stereo sample equals the operation applied to each channel as a mono sample.
- **SC-004**: Panning a stereo sample hard left produces silence on the right output and the left channel at full level; centre reproduces both channels as recorded.
- **SC-005**: A module with stereo samples opens in IT 2.14 without error.
- **SC-006**: Musicians can load a stereo loop and hear it in stereo with no more keystrokes than loading a mono sample (Enter at the requester).

## Assumptions

- Stereo means exactly two channels; files with more channels are treated as today (first channel / requester).
- The IT format's stereo flag and "left block then right block" layout are the interchange format, as OpenMPT and Schism Tracker use it; IT215 compression compresses each channel separately.
- XM, MOD, S3M, 669, MTM, PTM and FAR imports stay mono (those formats have no stereo samples).
- Balance pan and surround behaviour follow OpenMPT where IT has no rule; the README documents them.
- The engine stays IT's for mono samples; stereo is a separate, additional path (constitution I/III), documented as an extension.
- MIDI output and the Alt-F12 analyser are unaffected (they see the mix).
