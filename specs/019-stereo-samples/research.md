# Research: Stereo Samples

## R1 — The IT format's stereo samples

- **Decision**: sample flag bit 2 (0x04) = stereo; the data is Length frames of the left channel followed by Length frames of the right channel; Length, loop points and sustain loop points count frames. **it26 writes stereo samples uncompressed** whatever compression is selected; it reads compressed stereo as the left channel's compressed stream followed by the right's.
- **Evidence (2026-10-18)**: `testdata/openmpt-testpattern-stereo.it`, written by OpenMPT 1.32 (cwt 5132): every stereo sample occupies exactly Length × 2 channels × width bytes, and the data is planar (the snare's first block decays to RMS ≈ 1, the second starts with an attack at RMS ≈ 10,800). In three saves OpenMPT compressed the mono samples but wrote all stereo samples uncompressed — it26 follows that, so its files match OpenMPT's in this respect and nothing depends on an unverified compressed-stereo layout.
- **Rationale**: the layout OpenMPT and Schism Tracker read and write; it keeps IT's per-sample offsets and the left channel where IT 2.14 expects mono data.
- **Alternatives**: interleaved data (WAV style) — not what other IT writers produce.

## R2 — IT 2.14 with such a module

- **Decision**: documented, not engineered: IT 2.14's loader reads Length frames from the sample's offset (the left channel) and ignores flag 0x04; it26 writes nothing that would break that.
- **Rationale**: it26's own loader did exactly that until now (`s->Flags &= ~0x0C`, it_load.c ~399).

## R3 — Mixer branch

- **Decision**: in `M32MixHandler`'s voice loop, the sample header (`SAMPLEHDR(sc->SmpOffs)`, it_driver.c ~897) decides; stereo voices use a stereo inner loop: two `GetWaveform` reads (left / right block) with two filter states, left output from the left channel × left volume, right from the right × right volume, same ramp (LVC/RVC) and click-removal bookkeeping.
- **Rationale**: `MixInner` is one generic loop (cubic interpolation + resonant filter + ramp) for all modes (it_driver.c 288–330); a parallel stereo loop leaves the mono loop byte-identical.
- **Alternatives**: two voices per stereo note — doubles NNA/duplicate checks and the voice count, changes channel behaviour.

## R4 — Pan = balance

- **Decision**: no pan-law change. The engine already turns pan, pan envelope, channel pan and pan slides into a left and a right volume per voice; applying them to the left and right channel respectively gives balance: centre plays the image as recorded at the same loudness a mono sample would have (a stereo sample with equal channels sounds exactly like its mono version), hard left plays only the left channel at full volume.
- **Surround** (pan 100): the stereo sample's mono mix (L+R)/2 through IT's surround formula.

## R5 — Filters and NNA

- **Decision**: `static chnfilter_t FilterValuesR[MAXSLAVECHANNELS]` next to `FilterValues`; reset and copied wherever `FilterValues` is (reset in the filter setup per voice, copied on NNA at it_driver.c ~933).

## R6 — Loading paths

- **Decision**: `Load_SampleData` (it_load.c 270) is the one loader for module samples, ITS, ITI and the WAV/AIFF library (it_ris.c 894, 1595). Stereo is kept when (a) the IT/ITS header has flag 0x04 (planar in file; compressed = two streams) or (b) a standalone stereo file (Cvt & 32) and the requester answers Stereo (new return value 256). Deinterleave WAV data into the planar layout; the `Flags &= ~0x0C` rewrite keeps 0x04.
- **Requester**: `stereo_choice_prompt` (it_editor.c ~11660) gains a third button "Stereo", focused first; L/R keys and Esc keep IT's (Esc = Left).

## R7 — Editor audit

- **Decision**: every place computing a sample's byte size or walking `Data` gets the channel helpers. Counted sites: it_editor.c 64 uses of Length / data size, it_ris.c 37, it_load.c 14, it_save.c 7, it_import.c 6 (imports stay mono — check only), plus it_render.c (import, export). The task list enumerates them by function.

## R8 — New operation key

- **Decision**: Alt-U on F3. IT 2.14's F3 Alt keys (IT_OBJ1.ASM `SampleGlobalKeyList`, IT_I.ASM `SampleListKeys`) are A–J, L–O, Q–T, W–Y; K, P, U, V, Z are free. U for "unify" / channels; one key, its dialog depends on the sample.

## R9 — S3M export

- **Decision**: a temporary mono copy per stereo sample ((L+R)/2, rounded), the existing warning mechanism gains "Stereo samples converted to mono" (listed once).

## R10 — Size cap

- **Decision**: the per-slot cap (`SMP_MAXBYTES`, 4177920 bytes) counts both channels: a stereo sample may hold half the frames of a mono one. Imports from renders keep their 1 MB cap counting both channels.
