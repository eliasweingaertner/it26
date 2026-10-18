# Data Model: Stereo Samples

## Sample (`sample_t`, layout unchanged)

| Field | Meaning for stereo |
|---|---|
| Flags bit 0x04 | 1 = stereo (two channels), 0 = mono (as IT) |
| Flags bit 0x02 | 16-bit, for both channels |
| Length | frames per channel |
| LoopBeg/LoopEnd, SusLoopBeg/SusLoopEnd | frames (apply to both channels) |
| C5Speed | per frame |
| Data | one allocation, see below |

### In memory

```
mono:   [ channel 0: Length frames ][ pad 4 frames ]
stereo: [ left: Length frames ][ pad 4 ][ right: Length frames ][ pad 4 ]
```

- Channel c starts at `Data + c × (Length + 4) × width` (width 1 or 2).
- Padding: as the loader pads mono samples today (last value twice, then zeros), per channel, so the cubic interpolator never reads into the other channel.
- Size limit: `Length × width × channels ≤ SMP_MAXBYTES`.

### Invariants

- Both channels have the same Length, bit depth and loop points.
- Any operation either works on each channel the same way or replaces the whole sample.

## Voice (`slavechn_t`, layout unchanged)

The mixer reads the stereo flag from the voice's sample header each tick.
Right-channel filter state lives in a parallel array in the driver.

## Files

| Format | Stereo |
|---|---|
| IT module / ITS / ITI | flag 0x04; left block then right block; compressed: left stream then right stream |
| WAV / AIFF in | interleaved → planar (requester: Stereo / Left / Right) |
| WAV out (exports) | 2 channels, interleaved |
| S3M out | mixed to mono + warning |
| XM, MOD, S3M, 669, MTM, PTM, FAR in | mono, as now |
