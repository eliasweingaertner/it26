# Phase 1 Data Model: Info Page Views

## Editor-local state (static in `it_editor.c`, mirrors IT_DISPL.ASM)

| State | Original | Meaning |
|---|---|---|
| `dispwin_t DisplayWindows[5]` | `DisplayWindows` | `{u16 method; u8 topchan; u8 topline; u16 length}` (screen offset derived) |
| `NumWindows` (1..5) | `NumWindows` | default 3 with the 2.17 defaults |
| `CurrentWindow` | `CurrentWindow` | focused window |
| `ProcessWindow` | `ProcessWindow` | window being drawn (colour selection) |
| `InfoCurrentChannel` (0..63) | `CurrentChannel` | selected host channel |
| `InfoVelocity` | `Velocity` | 0 = velocity bars (default), 1 = volume bars |
| `InfoInstrumentNames` | `Instrument` | 0 = sample names (default), 1 = instrument names |
| `InfoFullScreen` | `FullScreen` | Ctrl-F: draw only CurrentWindow over rows 12..47 |

## Engine additions (`it_music.c`, 1:1)

| Function | Behaviour (from IT_MUSIC.ASM, verify at port time) |
|---|---|
| `Music_SoloChannel(uint16_t Channel)` | mute all channels except `Channel` via the mute table; if already soloed, restore |
| `Music_ToggleReverse(void)` | flip `ReverseChannels`, recalc pans |
| `Music_GetLastChannel(void)` | highest host channel used by the song |

## View methods

Method table indices 0..10 as research R2. Each method draws its own
boxes and content inside `(topline .. topline+length-1)`; channel-based
views clamp their `topchan` so `InfoCurrentChannel` is visible
(`Display_HostChannel1/2/22` clamp: topchan ≤ current ≤ topchan+len−3,
topchan ≤ 66−len).

## Invariants (from the original handlers)

- `NumWindows` ∈ 1..5; Ins requires current length > 6, halves it
  (upper half rounds up); Del merges the freed rows into the next window
  (or the previous when deleting the last).
- Resize keeps every window length ≥ 3 (border row not counted).
- Sum of lengths + toplines stays consistent: window i+1's topline =
  window i's topline + length (contiguous stack from row 12 to 47/50).
- Method cycling wraps mod 11.
- All engine reads (HChn/SChn/sample memory/pattern data) under
  `Engine_Lock`; solo/toggle go through engine calls.
