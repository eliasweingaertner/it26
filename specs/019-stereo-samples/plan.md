# Implementation Plan: Stereo Samples

**Branch**: master, local only until release | **Date**: 2026-10-18 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/019-stereo-samples/spec.md`

## Summary

Make stereo a first-class sample type. The IT format's stereo flag (sample
flag bit 2, left block then right block) becomes live: loaders keep both
channels, the mixer gets a stereo branch next to IT's mono path, savers
write the flag and both blocks (always uncompressed, as OpenMPT; reading
also accepts per-channel compressed blocks), and every sample operation in the editor goes through channel-aware
helpers. Songs without stereo samples never enter the new code, so their
playback stays bit-identical ([research.md](research.md) R1–R10).

## Technical Context

**Language/Version**: C11 (MSVC, GCC, Clang)

**Primary Dependencies**: none new

**Storage**: in memory, a stereo sample keeps two channel blocks in one allocation (left, interpolator padding, right, padding); on disk the IT/ITS/ITI layout (flag 0x04, left block then right block) and WAV (interleaved)

**Testing**: `test_pattern` (four modules `IDENTICAL`, the mono-path gate); `ITED_SELFTEST` new block **STR** (the OpenMPT reference `testdata/openmpt-testpattern-stereo.it`: load, play, save with and without compression selected, reload; synthetic samples for mono-next-to-stereo, instrument mode and compressed-stereo reading; every F3 operation per channel; balance pan output; S3M downmix; render import)

**Target Platform**: Windows, Linux, macOS

**Project Type**: desktop application (`ited`; `itplay` plays stereo samples too, through the same mixer)

**Performance Goals**: a stereo voice costs about twice a mono voice in the mixer; mono voices unchanged

**Constraints**: mono path byte-identical (constitution I/III); `slavechn_t` and `sample_t` layouts pinned (`_Static_assert`) — no new fields; per-slot size cap counts both channels

**Scale/Scope**: up to 99 samples, any of them stereo; 256 voices

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Engine fidelity (I) + Determinism (III)**: PASS with justified extensions. Touched engine files: `it_driver.c` (a stereo branch in the mixer + a second filter state per voice), `it_load.c` / `it_save.c` (stereo flag and blocks; stereo IT215 per channel). IT's mono code paths are not changed; the stereo branch is taken only when a voice's sample has flag 0x04, which no IT 2.14 song has. `test_pattern` must stay `IDENTICAL` with the known hashes. No change to `it_music.c`, `it_effects.c`, `it_tables.c`, struct layouts.
- **Authentic data (II)**: PASS with notes. New UI (the third "Stereo" button in "Loading Stereo Sample", the Alt-U dialog, the two-lane waveform, "Stereo" in the sample info) has no IT original; it follows IT's existing box styles and the requester it extends. Flagged as extension UI in the README.
- **One real engine (IV)**: PASS. One mixer; stereo is a branch in it.
- **Portability (V)**: PASS. Plain C.

**Post-design re-check**: PASS — no layout changes (stereo state lives in the sample flag and in a parallel filter array in `it_driver.c`).

## Project Structure

### Documentation (this feature)

```text
specs/019-stereo-samples/
├── spec.md
├── plan.md
├── research.md          # R1–R10
├── data-model.md        # in-memory layout, flags, file layouts
├── quickstart.md
├── contracts/
│   ├── file-formats.md  # IT / ITS / ITI / WAV / S3M behaviour
│   └── keys-and-ui.md   # requester, Alt-U, F3 display, help
├── checklists/requirements.md
└── tasks.md             # /speckit-tasks
```

### Source Code

```text
src/
├── it_driver.c   # mixer: stereo branch in MixInner, right-channel filter state,
│                 #   NNA copy of both filter states, surround = mono mix
├── it_load.c     # Load_SampleData: keep stereo (WAV interleaved -> planar,
│                 #   IT planar, IT215 per channel); stereo choice "Stereo"
├── it_save.c     # raw + IT215 writers per channel; flag 0x04; S3M downmix + warning
├── it_ris.c      # ITS/ITI/WAV save stereo; library loads through Load_SampleData
├── it_render.c   # stereo imports (memory sink 2 channels), stereo sample export
├── it_editor.c   # channel helpers; every sample op; Alt-U; F3 waveform/info;
│                 #   requester third button; hot swap; help; selftest STR
└── it_structs.h  # comment only: flag 0x04 = stereo (no layout change)
README.md, CHANGELOG.md, docs/HANDOFF.md
```

## Design

### Memory layout (data-model.md)

One allocation: `[L: Length frames][pad 4][R: Length frames][pad 4]`
(bytes per frame 1 or 2). `SMP_CH(s, c)` gives channel c's start;
`smp_data_bytes(s)` = Length × width × channels. The padding keeps the
cubic interpolator (reads pos+1, pos+2) inside each channel.

### Mixer

- Per voice in the mix loop: `stereo = SAMPLEHDR(sc->SmpOffs)->Flags & 4`.
- Mono: unchanged code.
- Stereo, normal: `xL = GetWaveform(L, …, &FilterValues[i])`,
  `xR = GetWaveform(R, …, &FilterValuesR[i])`; left output −= xL × LVC,
  right output −= xR × RVC (same ramping). This *is* balance: the engine's
  left/right volumes come from pan as for mono.
- Stereo, surround: x = (xL + xR) / 2 through IT's surround formula.
- Click removal keeps LastLeftValue / LastRightValue as now.
- `FilterValuesR[]` reset where `FilterValues[]` is, copied with it on NNA
  (it_driver.c ~933).

### Loading / saving

- `Load_SampleData`: IT samples with flag 0x04 → read Length frames for
  each channel (planar in the file); compressed → decompress the left
  stream, then the right stream; WAV/AIFF stereo (Cvt & 32) → requester
  `Load_StereoChoice` returns 64 left / 192 right / **256 stereo**; stereo
  deinterleaves into the two blocks. The `Flags &= ~0x0C` rewrite keeps
  0x04 when stereo is kept.
- Savers: stereo = left block then right block, always uncompressed (as
  OpenMPT; mono samples keep the chosen compression); ITS/ITI the same;
  S3M: mix down
  (L+R)/2 into a temporary mono copy + a warning line "Stereo samples
  converted to mono".
- WAV export (RIS_SaveWAV, Render_SaveSampleWav): 2 channels, interleaved.

### Editor

- Channel helpers replace every `Length << is16` byte computation and
  direct `Data` walk (audit list in research R7, ~110 sites).
- Operations run per channel (reverse, invert, centralise, amplify, cut,
  cut before loop, resize ± interpolation, sign conversion) or on the whole
  sample (delete, copy, replace, swap, exchange, hot swap).
- Alt-U (unused on F3 in IT 2.14): stereo → "Convert to mono: Mix / Left /
  Right"; mono → "Convert to stereo?" (both channels = the mono data).
- F3 waveform: two lanes (left top, right bottom) in the existing box;
  sample info shows "Stereo" next to the bit depth.
- Requester "Loading Stereo Sample": Stereo / Left / Right, Stereo focused.
- Renders (018): import jobs keep 2 channels when the song is stereo.

## Complexity Tracking

| Item | Why needed | Simpler alternative rejected because |
|---|---|---|
| Stereo branch in the mixer (engine file) | stereo can't be played without it | a second mixer would violate "one real engine"; mixing stereo as two voices would double NNA/duplicate-check logic and break channel limits |
| Parallel `FilterValuesR[]` | `slavechn_t` is layout-pinned | adding a field breaks the pinned struct |
| Planar layout with padding between channels | interpolation reads past a channel's end | interleaved would change every mono-assuming read and the file layout conversion |
