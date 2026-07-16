# Data Model: Standalone WAV Sample Loading (008)

No new persistent entities. The feature maps one on-disk format onto the
existing feature-006 structures. Field derivations are fixed by
[research.md](research.md) R1/R2; this file is the lookup table.

## WAV source file (input)

Byte offsets are absolute file offsets, little-endian.

| Offset | Size | Field | Constraint (identification) |
|--------|------|-------|------------------------------|
| 8      | 8    | `"WAVEfmt "` | must match (offset 0 `RIFF` NOT checked) |
| 16     | 2    | fmt chunk size, low word | used as the walk base only |
| 18     | 2    | fmt chunk size, high word | must be 0 |
| 20     | 2    | wFormatTag | must be 1 (integer PCM) |
| 22     | 2    | nChannels | 2 → stereo; anything else → mono |
| 24     | 2    | nSamplesPerSec, low word | → `C5Speed` (16-bit only) |
| 34     | 1    | wBitsPerSample | must be 8 or 16 |
| 20+fmt | —    | chunk walk | ≤3 chunks; skip = `low_word(size)+8`; find `"data"` |

`data` chunk: size read as full dword for the length; payload starts at
size-field offset + 4.

## Library record: `slibent_t` (existing, `it_ris.h`)

| Field | WAV value |
|-------|-----------|
| `Format` | 5 (8-bit) / 7 (16-bit) |
| `FileSize` | capped data byte size |
| `SrcFile` | source path |
| `hdr` (`sample_t`) | see below |

## `slibent_t.hdr` (`sample_t`, ITS-shaped)

| Field | WAV value |
|-------|-----------|
| `SampleName` | base filename, ≤13 chars, NUL-padded to 26 |
| `GvL` / `Vol` | 64 / 64 |
| `Flags` | `1 \| (16bit?2:0) \| (stereo?4:0)` |
| `Cvt` | `(16bit?1:0) \| (stereo?32:0)` |
| `DfP` | 0 |
| `Length` | `min(data_size_dword, 4177910) >> 16bit >> stereo` (frames) |
| `LoopBeg/LoopEnd/Sus*` | 0 |
| `C5Speed` | `word[24]` (low 16 bits of sample rate) |
| `Vi*` (vibrato) | 0 |
| `OffsetInFile` | data payload offset |
| `Data` | NULL until rip |

## Conversion flags at load (`Load_SampleData`, existing + new bit)

| `Cvt` bit | Meaning | WAV usage |
|-----------|---------|-----------|
| 0 | data is signed (clear → XOR 0x80/0x8000 pass) | set for 16-bit, clear for 8-bit |
| 1 | byte-swap (16-bit BE) | never for WAV |
| 2 | delta / IT215 second pass | never for WAV |
| 3 | byte-delta (PTM) | never for WAV |
| 5 | **stereo (new)** — read `len<<is16<<1` bytes, convert, keep left channel in-place | set when nChannels == 2 |

Post-load (unchanged): `Flags &= ~0x0C; Cvt = 1;` — result is always flat
signed mono in memory.

## State transitions

Same as every feature-006 sample source:

```
requester file list ──Enter──▶ RIS_ScanModule ──1 record, fmt 5/7──▶ direct
  lib_load_sample_entry ──"Replace sample N?" if occupied──▶ stop_song +
  Engine_Lock ──▶ RIS_LoadSample (Load_SampleData) ──▶ slot install ──▶ unlock
```

Preview: note key on the record → same rip into check slot 100 →
`Music_PlaySample`.
