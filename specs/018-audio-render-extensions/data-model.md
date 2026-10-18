# Data Model: Audio Rendering Extensions

## Render job

| Field | Values | Notes |
|---|---|---|
| kind | pattern, song | pattern = one pass (R1); song = order 0 to the song end, 30-minute cap |
| pattern | 0..199 | refused at ≥ 200 |
| rows | 1..200 | from the pattern header; 0 → 64 |
| solo | −1 or channel 0..63 | stems: every other channel muted for the pass |
| destination | import, file | import → 44.1 kHz, 16-bit, memory; file → Shift-F5 rate, 32-bit float unclamped |
| channels | 1 or 2 | 2 when the song's stereo flag is set |
| name | 8.3 string | see "File names" |
| resume | yes / no | yes only for single-pattern import/file renders started by a gesture |

**State transitions**: idle → validating destination (bad folder → idle with
error, nothing touched) → live audio gated + play state saved → rendering
(Esc / second gesture → finishing) → finishing (file sizes written / sample
built) → driver and settings restored → resumed (if armed) → idle.

## Saved play state (for resume)

PlayMode (0 / 1 pattern / 2 song), CurrentOrder, CurrentRow, CurrentPattern,
NumberOfRows. Resume: mode 1 → play that pattern from that row; mode 2 →
play the song from that order and row; mode 0 → nothing.

## Imported sample (slot N)

| Field | Value |
|---|---|
| slot N | max(highest used sample, highest used instrument) + 1, ≤ 99 |
| flags | present, 16-bit, forward loop; always mono (stereo mix: (L+R)/2) |
| length | rendered frames, at most 1 MB of data |
| loop | 0 .. length; sustain loop off |
| C5 speed | 44100 |
| default / global volume | 64 / 64 |
| default pan | off (32 stored) |
| sample name | "Pattern NNN" |
| file name field | the render's 8.3 name |
| host instrument | instrument mode only: same number if blank, else first blank; all 120 notes → N |

## Quicksave folder

`quicksavedir=` in `ited.cfg`, up to 64 characters, empty by default.
Render folder resolution: Quicksave (must exist) → F12 sample directory →
current folder.

## Render counter

Per-session integer, 0 at start, incremented before each counter-named
render; formatted as 4 digits.

## File names (Esa's rules)

| Render | Name |
|---|---|
| single pattern (import or Quicksave) | `LL` + HHMMSS + `.WAV` |
| Shift-Alt-M per channel, F10 WAV/MWAV without a typed name, batch | PFX + NNNN + `.WAV` (PFX: song name's first 3 chars upper-cased, non-alphanumeric → X, padded with X; empty → PTN) |
| F10 WAV with typed name | typed name upper-cased, `.WAV` added if no extension |
| F10 MWAV with typed name | base cut to 6 chars + 2-digit channel (1-based) + extension |
| sample export | HHMMSS + 2-digit slot + `.WAV` |
