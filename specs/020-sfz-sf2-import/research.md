# Research: SFZ and SoundFont 2 Instrument Import

## R1. What the real material uses (survey of `D:\CommonHome\sound\sfz`)

1387 .sfz files; sample files: 7501 FLAC, 3369 WAV, 528 AIFF, **0 Ogg**.
Opcode counts (top): `<region>` 135006, `sample` 135005, `hikey`/`lokey`
~105k, `pitch_keycenter` 104764, **`seq_position` 100128**, `ampeg_release`
10166, `<group>` 8264, `key` 8157, `group`/`off_by` ~7k, **`loccN`/`hiccN`
6120**, `lovel`/`hivel` ~5.9k, `pan` 4621, `offset` 4488, `lorand`/`hirand`
~3.8k, **`sw_last`/`sw_lokey`/`sw_hikey` ~3.2k**, `amp_velcurve_N` 2665,
`ampeg_attack` 1961, `cutoff` 1886, `<global>` 1877, `fil_type` 1840,
**`#include` 1327**, `volume` 1137, `tune` 659, `trigger` 605, `loop_mode`
585, `sw_default` 212, `ampeg_decay` 182, `ampeg_sustain` 254,
`pitch_keytrack` 159, `eg1_*` (SFZ v2 flex EG) a few hundred.

- **Decision**: support `#include`, `#define`, `<control>`/`<global>`/`<master>`/`<group>`/`<region>`, and the opcodes listed in data-model.md. Ignore the rest (EQ, LFOs, flex EGs, velocity curves, CC modulation, `group`/`off_by`, `delay*`).
- **Decision**: CC conditions are *evaluated* (controller values default to 0, except `set_ccN`; CC 7 = 100? — sfizz defaults all to 0; we do the same), not skipped: skipping would empty many instruments (6120 conditions).
- **Decision**: key switches: active switch = `sw_default` if any region/group sets it, else the lowest `sw_last` used. Regions with `sw_last` ≠ active are out; `sw_lokey`/`sw_hikey` only describe the switch range and are otherwise ignored; regions with `sw_down`/`sw_up`/`sw_previous` are out.
- **Decision**: `seq_position` > 1 out; `lorand` > 0 out (the "first" random alternative is the one containing 0); `trigger` = `release`/`release_key`/`legato` out (`attack`/`first` in).

## R2. Velocity and layer choice

IT notes play at full volume by default (volume 64), the counterpart of
velocity 127. **Decision**: among regions passing R1 for a key, those whose
velocity range contains the highest velocity any of them reaches win
(normally 127); ties → first in file order. Same for SF2 zones (velRange).
Alternative "velocity 100" rejected: arbitrary; a piano's loudest layer is
what IT's default volume implies.

## R3. Pitch

IT plays note *N* of a sample at `C5Speed · 2^((N−60)/12)`. MIDI key *k* →
IT note *k* (MIDI 60 = C-5, as OpenMPT/Schism). For a region with root key
*R* (SFZ `pitch_keycenter`, default 60; SF2 `overridingRootKey` else the
sample's `byOriginalPitch`) and total tuning *t* cents (SFZ `transpose`·100
+ `tune`; SF2 `coarseTune`·100 + `fineTune` + sample `chPitchCorrection`),
the source plays at `rate · 2^(t/1200)` at key *R*, so

`C5Speed = round(rate · 2^((60 − R)/12 + t/1200))`.

Note table identity (key → same note). Key tracking ≠ 100 % ignored.
Exactness: C5Speed is integer Hz → error < 1 cent for rates ≥ 1733 Hz.

## R4. SoundFont 2.04 structure (facts used)

RIFF `sfbk`: LIST `INFO`, LIST `sdta` (`smpl` 16-bit LE mono data, optional
`sm24`), LIST `pdta` with `phdr`(38 B) `pbag`(4) `pmod`(10) `pgen`(4)
`inst`(22) `ibag`(4) `imod`(10) `igen`(4) `shdr`(46); each list ends in a
terminal record. Generators used (index): 0 startAddrsOffset, 1
endAddrsOffset, 2 startloopAddrsOffset, 3 endloopAddrsOffset, 4
startAddrsCoarseOffset, 8 initialFilterFc, 9 initialFilterQ, 12
endAddrsCoarseOffset, 17 pan, 33..38 volEnv delay/attack/hold/decay/
sustain/release, 41 instrument, 43 keyRange, 44 velRange, 45
startloopAddrsCoarseOffset, 48 initialAttenuation, 50
endloopAddrsCoarseOffset, 51 coarseTune, 52 fineTune, 53 sampleID, 54
sampleModes, 58 overridingRootKey.

- Zone layering: an instrument's first zone without `sampleID` is its global zone (defaults for the others); a preset's first zone without `instrument` is its global zone. Preset-level values **add** to instrument-level values for the additive generators (tuning, pan, attenuation, envelope, filter); key/vel ranges **intersect**; sample-address, `sampleModes`, `overridingRootKey` are instrument-level only.
- Defaults: filterFc 13500 (≈ 19.9 kHz = off), envelope times −12000 timecents (≈ 1 ms), sustain 0 cB, rootKey −1.
- `sampleModes`: 0/2 no loop, 1 loop, 3 loop while held then play to the end → **IT sustain loop**.
- `initialAttenuation` is applied ×0.4 (the de-facto EMU/FluidSynth convention; files are authored for it).
- Stereo: `sfSampleType` 2 = right, 4 = left, `wSampleLink` = partner. Presets reach stereo through two zones with the same ranges, one per side (pans ±500). **Decision**: when the chosen zone's sample is linked and a zone for the same key and velocity uses the partner, build one stereo IT sample (left = the left-type sample, length = the shorter, loops/tuning from the chosen zone, pan centre).
- SF3 (`ifil` major 3, Ogg data): presets list; loading is refused with "SF3 not supported".
- Large files: the scanner walks RIFF chunk headers with `fseek` and reads only `pdta` (+ `ifil`); sample data is read per sample at load time (FR-014).

## R5. SFZ syntax (facts used)

Headers `<control>`, `<global>`, `<master>`, `<group>`, `<region>` (others
like `<curve>`, `<effect>`, `<midi>` are skipped with their opcodes).
Opcode values run to the next `name=` token or header, so `sample=` values
may contain spaces. `//` line comments and `/* */` block comments.
`#define $NAME value` substitutes `$NAME` in later text; `#include "file"`
inserts a file relative to the root .sfz's directory. `default_path` (in
`<control>`) prefixes sample paths; paths use `\` or `/`. Note names
`c4` = 60 (`c-1` = 0), sharps `#`, flats `b`. `key=` sets lokey, hikey and
pitch_keycenter. `loop_mode`: `no_loop`, `one_shot`, `loop_continuous`,
`loop_sustain`; `loop_start`/`loopstart`, `loop_end`/`loopend` (inclusive
end). When `loop_mode` is unset the sample file's loop is used (SFZ spec:
loop_continuous if the file has a loop). `offset`, `end` (inclusive; `end=0`
or `-1` = silent region → skipped). `volume` dB, `pan` −100..100, `tune`
cents, `transpose` semitones, `cutoff` Hz, `resonance` dB, `fil_type`
(lpf_1p/lpf_2p/lpf_4p/lpf_6p → low-pass; other types → filter ignored),
`ampeg_delay/attack/hold/decay/sustain(%)/release` seconds.

## R6. Audio decoding

- **Decision**: WAV and AIFF via `ma_dr_wav_init_file_with_metadata` + `ma_dr_wav_read_pcm_frames_s16` (all PCM widths, float, A-law/µ-law, AIFF/AIFC); FLAC via `ma_dr_flac_open_file` + `ma_dr_flac_read_pcm_frames_s16`. All data ends up 16-bit signed (8-bit WAVs grow to 16-bit; IT stores 16-bit fine). Mono → mono IT sample; ≥ 2 channels → stereo IT sample from the first two.
- These decoders exist in miniaudio only without `MA_NO_DECODING`; it is dropped at the single implementation site (`it_editor.c`) and `MA_NO_MP3` added; `it_sfimport.c` includes the header with the same macros (declarations only). Vendored file untouched (constitution V).
- WAV loop: `smpl` metadata, first loop (forward/backward → loop, ping-pong → ping-pong loop); end is inclusive → `LoopEnd = last + 1`.
- AIFF loop: dr_wav doesn't expose AIFF markers. **Decision**: a ~40-line `MARK` + `INST` chunk reader (sustainLoop: playMode 1 forward / 2 ping-pong, begin/end marker ids → positions).
- Unsupported (Ogg, MP3, missing file) → region skipped, counted.
- Path resolution: as written; if it fails on a case-sensitive file system, a case-insensitive per-component directory search (POSIX only; Windows is case-insensitive already).

## R7. Deduplication and reduction

IT sample identity = (source, start, end, loop mode/start/end, C5Speed,
GvL, pan). Same identity → one IT sample.

**Reduce to fit** when unique samples *U* > free slots *F* (counted after
releasing the target's exclusive samples): sort the unique samples by root
key (then first key used), keep indices `round(i·(U−1)/(F−1))` for
i = 0..F−1 (F = 1 → the one nearest C-5), and reassign every key to the kept
sample whose root key is nearest (ties → lower). Pitch stays exact because
each sample's C5Speed encodes its own root. Status: "reduced from U to F
samples". Alternatives: refuse (useless for pianos: 88+ samples > 99 − used),
drop top/bottom (loses range).

## R8. Envelope (amp EG → IT volume envelope)

IT envelope ticks at the song's tick rate; at IT's default tempo 125 that is
50 ticks/s. Representative region: the one chosen for C-5 (MIDI 60), else
the first chosen. SF2 times are timecents (`s = 2^(tc/1200)`), sustain is
attenuation in cB (`level = 10^(−cB/200)`); SFZ times are seconds, sustain
percent.

Nodes (magnitude 0..64, tick ≤ 9999, max 25):
`(0,0)` → delay `(d,0)` → attack `(d+a,64)` → hold `(…,64)` → decay to
sustain level S with exponential shape (points at 10 %, 30 %, 60 %, 100 %
of the time, `64·(S + (1−S)·10^(−3·x))`, last exactly S) → sustain point
(SLB = SLE = that node) → release from S to 0 (same shape, end at 0).
Ending on 0 makes the engine cut the voice (`UpdateEnvelope` end, value 0
→ `TurnOff17`, it_music.c). Instrument `FadeOut` = 0 when the envelope is on.
Zero-length segments collapse (a node only when its tick > previous tick;
the attack peak replaces (0,0) when attack = 0 and delay = 0).

Flat envelope (no delay, attack < 5 ms, sustain 100 %, decay irrelevant,
release < 50 ms): envelope off and `FadeOut = clamp(ceil(1024 / (release·50)), 1, 256)` — the engine subtracts FadeOut from a 1024 counter each tick after note-off.

## R9. Filter

it_driver.c: `r = 2^(−freqvalue/(24·256)) · MixSpeed/(2π·110·2^0.25)`,
`freqvalue ≈ 256·cutoff` without a filter envelope ⇒ cutoff c ↔
`f = 130.81·2^(c/24)` Hz (c=127 ≈ 5.13 kHz). Resonance: QualityFactorTable
= `10^(−r·24/(128·20))` ⇒ `r = dB·128/24`.

- **Decision**: `c = round(24·log2(f/130.81))`; if c ≥ 127 → filter off (IFC/IFR left 0) unless resonance > 0, then IFC = 127|80h; else IFC = c|80h, IFR = (clamp(r,0,127))|80h when resonance > 0. Source f: SFZ `cutoff` (Hz), SF2 `8.176·2^(Fc/1200)`; resonance: SFZ dB, SF2 cB/10.

## R10. Volume and pan

`GvL = clamp(round(64·10^(dB/20)), 0, 64)` with dB = SFZ `volume`, SF2
`−0.4·attenuation/10`. Positive gain clips at 64 (IT has no boost; GbV
128 is unity). Pan: SFZ −100..100 / SF2 −500..500 → `DfP = 80h | round(32 +
32·p)`; centre → DfP 0 (no override, channel pan applies). Instrument:
GbV 128, DfP 80h|32 (off), NNA cut (0) — like a fresh F4 instrument.
Sample: Vol 64.

## R11. Load Instrument record formats

Existing: 1 dir, 3 ITI, 4 XI, 5 in-IT, 6 in-XM, 8 IT library, 9 XM library;
`(Format & 8)` = module group in the sort; `li_enter` used `>= 8`.
**Decision**: `RI_FMT_SFZ = 0x10`, `RI_FMT_SF2PRESET = 0x11`,
`RI_FMT_SF2 = 0x18`, and `RI_IsLibrary(f) = (f & 8)`; every `>= 8` test in
the Load Instrument code becomes `RI_IsLibrary`. SF2 is identified by
content (`RIFF....sfbk`), SFZ by the `.sfz` extension (no magic).

## R12. Out-of-sample check

`lib_load_instrument_entry` refuses when `NumSamples > LibUnused`. For the
new formats the converter itself reduces to fit, so the check becomes
"at least one sample slot free after the release" and is done inside the
loader (error code `RI_ERR_SLOTS` → IT's "Out of sample space!").
