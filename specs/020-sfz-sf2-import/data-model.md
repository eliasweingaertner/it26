# Data Model: SFZ and SoundFont 2 Instrument Import

## sfi_region_t (intermediate, one per SFZ region / SF2 zone pair)

| Field | Meaning | SFZ source | SF2 source |
|---|---|---|---|
| lokey, hikey | key range 0..127 | `lokey`/`hikey`/`key` (default 0..127) | keyRange ∩ |
| lovel, hivel | velocity range | `lovel`/`hivel` (1..127 → 0..127) | velRange ∩ |
| root | root key | `pitch_keycenter` (`key`, default 60; `sample` = file's unity note) | overridingRootKey ≥ 0 else byOriginalPitch |
| tune | cents | `transpose`·100 + `tune` | coarse·100 + fine + chPitchCorrection |
| src | sample source | resolved file path | sample index (+ linked partner index) |
| start, end | frame range | `offset`, `end`+1 (default whole file) | shdr start/end + address offsets |
| loopmode | 0 none, 1 loop, 2 sustain, 3 ping-pong | `loop_mode` / file loop | sampleModes |
| loopstart, loopend | frames (end exclusive) | `loop_start`/`loop_end`+1 / file | shdr loop + offsets |
| gain_db | volume | `volume` | −0.04·attenuation |
| pan | −1..1 | `pan`/100 | pan/500 |
| env | delay, attack, hold, decay, sustain (0..1), release (s) | `ampeg_*` | volEnv gens |
| cutoff_hz, reso_db | low-pass (0 = none) | `cutoff`, `resonance` if `fil_type` low-pass | Fc, Q |
| order | file order | region index | zone index |

Validity (selection, research R1/R2): `trigger` attack/first; `seq_position` ≤ 1;
`lorand` ≤ 0; CC conditions true at defaults; key switch = active switch;
`end` > `offset`; sample decodable (checked at load; failures counted).

## sfi_voice_t (one IT sample to create)

Unique (src, start, end, loopmode, loopstart, loopend, c5speed, gvl, dfp).
Fields: the region it came from, `c5speed` (research R3), `gvl`, `dfp`,
`stereo_partner` (SF2), `keys` bitmap of IT notes using it.

## Converted output

- `instrument_t`: ID "IMPI", name (≤ 25 chars), NoteSampleTable[k] = (k, voice slot or 0), VEnvelope (R8), PEnvelope/PtEnvelope defaults as `xi_chain`, FadeOut (R8), GbV 128, DfP 0xA0, IFC/IFR (R9), PPC 60, NoS = voices.
- `sample_t` per voice: ID "IMPS", name (file stem / SF2 sample name, ≤ 25), DOSFileName (12 chars), GvL, Vol 64, DfP, Flags 1|2 (+4 stereo, +0x10 loop, +0x20 sus loop, +0x40/0x80 ping-pong), Cvt 1, C5Speed, loops, Data (16-bit, it_sample.h layout, padded via `smp_pad`).

## Load Instrument record (ilibent_t) additions

| Format | Meaning | Name | NumSamples | Offset |
|---|---|---|---|---|
| 0x10 | .sfz file | file stem | voices needed (before reduction) | 0 |
| 0x11 | SF2 preset | "bbb:ppp name" | voices needed | preset index |
| 0x18 | .sf2 file (library) | "Library" marker | 0 | 0 |
