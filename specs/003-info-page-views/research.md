# Phase 0 Research: Info Page Views

## R1. "The real oscilloscope" — what IT 2.17 actually draws

**Finding**: `Display_HostChannel` (IT_DISPL.ASM 466) draws, per active
channel, a **velocity bar**: it takes the slave channel's sample span
mixed since the last frame (`OldSampleOffset` +2Ch → `SampleOffset` +4Ch,
adjusted for loop wrap and 16-bit samples), scans the actual sample bytes
for min/max, and displays `(max−min) * FV >> 8` (rounded) as a bar of
chars 176/179/182 with a 173+n fractional tip, attr 05h (01h muted).
With `Velocity=0` off ('V' key), the bar is plain `FV` (volume bars).
`Music_GetOutputWaveform` is an unresolved dead extern; no view renders
mixer output. **Decision**: port the velocity/volume bars exactly;
document in the README fidelity notes that "oscilloscope" in the spec
maps to IT's velocity bars (there is nothing else in 2.17).

## R2. The view-method set (DisplayDataModes, PLAYMETHODS = 11)

| # | Proc | Line | View |
|---|---|---|---|
| 0 | `Display_HostChannel` | 466 | track view (sample/ins names, velocity/volume bar, pan) |
| 1 | `Display_5Channel` | 2094 | 5-channel pattern view (" Channel xx " headers) |
| 2 | `Display_8Channel` | 2310 | 8-channel ("  xx  ") |
| 3 | `Display_10Channel` | 2546 | 10-channel |
| 4 | `Display_18Channel` | 2801 | 18-channel ("xx") |
| 5 | `Display_24Channel` | 2900 | 24-channel |
| 6 | `Display_36Channel` | 3128 | 36-channel |
| 7 | `Display_64Channel` | 3305 | 64-channel |
| 8 | `Display_Variables` | 3365 | "Active Channels: n (v) / Global Volume: g" |
| 9 | `Display_NoteDots` | 3429 | note dots (DOTSDISPLAY) |
| 10 | `Display_Details` | 998 | per-channel NNA/freq/position/volumes table |

`Display_SampleDots` (3573) is inside a `Comment ~ ~` block — **not in
the 2.17 build**. Excluded (spec deviation documented).

## R3. The window system (DisplayWindows)

Entry = 8 bytes: `{u16 method; u8 topchannel; u8 topline; u16 length;
u16 screenoffset(=topline*160)}`; max **5** windows (`DisplayInsert`
caps at 5 and needs current length > 6), min 1. Defaults (2.17):
`{0, 0, 12, 20}`, `{8, 0, 32, 3}`, `{5, 0, 35, 15}` — track view,
Variables, 24-channel. Keys (DisplayListKeys):

| Key | Handler | Effect |
|---|---|---|
| Up/Left, Down/Right | DisplayUp/Down | CurrentChannel ±1 (0..63) |
| Home/End | DisplayHome/End | channel 0 / Music_GetLastChannel |
| PgUp/PgDn | DisplayPageUp/Down | cycle window's method −/+ (mod 11) |
| Tab / Shift-Tab | DisplayNext/Previous | CurrentWindow +/− |
| Ins / Del | DisplayInsert/Delete | split current window (len>6) / merge |
| Alt-Up/Alt-Down | DisplayAltUp/Down | move the border above/below (min len 3) |
| '+' / '-' | DisplayPlus/Minus | Music_NextOrder / Music_LastOrder |
| Alt-F9, shift-'Q' | DisplayToggleChannel | Music_ToggleChannel(CurrentChannel) |
| Alt-F10, shift-'S' | DisplaySoloChannel | Music_SoloChannel(CurrentChannel) |
| Alt-R | DisplayToggleReverse | Music_ToggleReverse |
| Alt-S | DisplayToggleStereo | flip stereo flag + Music_InitStereo |
| shift-'G' | Display_GotoPattern | jump pattern editor to played pattern/row |
| shift-'V' | DisplayToggleVelocity | velocity vs volume bars |
| shift-'I' | DisplayToggleInstrument | instrument vs sample names |
| Space | Display_SpaceBar | (goto pattern + switch to F2, per proc) |
| Ctrl-F | Display_FullScreen | fullscreen current window (O1_FullDisplayList) |
| Alt-F12 | Fourier spectrum analyser | **out of scope** (IT_FOUR.ASM, own feature) |

Window drawing quirk (`DrawDisplayData`): every window except window 0 —
and any window in fullscreen — whose method is neither 0 (HostChannel)
nor 9 (NoteDots) is drawn one row taller (topline−1, len+1) so its box
top border overwrites the previous window's bottom border; restored
after the call.

**Alt-key limitation**: the port's key layer has no Alt modifiers yet
(pre-existing; HANDOFF roadmap #5). Where the original offers both an
Alt combo and a shifted-letter alias ('Q','S'), the alias is ported; the
Alt-only ones (Alt-R reverse, Alt-S stereo, Alt-Up/Down resize) get
substitutes only if a conflict-free plain key exists — otherwise they
are flagged "needs Alt" on the info line and documented. Resize is
essential to US3, so map it to Ctrl-U/Ctrl-D (documented deviation until
Alt support lands).

## R4. Colours (GetChannelColour, IT_DISPL 346)

Channel-number gutter: current channel in the *active* window 13h,
current in inactive window 12h→10h, muted 16h/11h; numbers via
`DrawChannelNumbers` (attr 23h current / 21h / 20h inactive window /
26h muted-current). Variables text 20h, 23h when its window is current.
NoteDots: dot glyphs 193..201 sized by `(FV+7)>>4`, attr = 2 +
(sample&3) (or instrument&3 for MIDI sample 100), muted = 1; disowned
channels only overwrite smaller dots; empty cell = glyph 193 attr 6.

## R5. Engine additions (1:1 ports required)

- `Music_SoloChannel` (IT_MUSIC.ASM): solo/unsolo via the mute table —
  read the proc during implementation and port literally; drives
  `MuteChannelTable` + recalc, same as Music_ToggleChannel.
- `Music_ToggleReverse` (IT_MUSIC.ASM): flips `ReverseChannels` (+ pan
  recalc). Trivial.
- `Music_GetLastChannel`: highest used channel — check if derivable from
  existing state; port if present in IT_MUSIC.ASM.
- `Music_NextOrder`/`Music_LastOrder`: skip to next/previous order while
  playing — port if small; else '+'/'-' flash "not ported yet" (they are
  conveniences, not part of the spec's success criteria).

## R6. Multi-channel pattern views

`Display_5/8/10/18/24/36/64Channel` render the *playing pattern's* rows
centred on the current row, per channel column, decoding packed pattern
data (`DataDecode`, `DecodeOffset/Row/MaxRow` state machine mirroring the
player's repeat-bit decoder). The port already has the exact decoder
(`it_pattern.c` / `Pattern_Unpack`); the views will decode via a
row-window unpack of the playing pattern under lock, then format each
cell per the proc (note/ins/vol/effect columns at the density the view
defines — read each proc during implementation for exact columns/attrs).

## R7. Idle/update model

`DisplayUpdateScreen` redraws when play position changes; the port
redraws every frame already — equivalent. `Display_GotoPattern`/
`Display_SpaceBar` reuse the existing editor pattern-follow helpers.
