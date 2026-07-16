# Phase 1 Data Model: F4 Instrument Editor

No new persisted data. All state is the engine's existing `instrument_t`
(554 bytes, pinned by `_Static_assert` in `it_structs.h`) plus editor-local UI
state.

## Entities

### Instrument (existing — `instrument_t`, edited in place)

| Field | Offset | UI binding (tab) |
|---|---|---|
| `NNA` | 0x11 | General: NNA radio group (Cut/Continue/Off/Fade) |
| `DCT` | 0x12 | General: DCT radio group (Off/Note/Sample/Instrument) |
| `DCA` | 0x13 | General: DCA radio group (Cut/Off/Fade) |
| `DOSFileName` | 0x04 | General: filename string input (12 chars, box (55,46)-(73,48)) |
| `NoteSampleTable[240]` | 0x40 | General: note-translation window (120 note→(sample,note) pairs) |
| `FadeOut` | 0x14 | Volume: scalable bar 0..256 |
| `GbV` | 0x18 | Volume: bar 0..128 |
| `RV` | 0x1A | Volume: Volume Swing bar 0..100 |
| `DfP` | 0x19 | Panning: toggle bit 0x80 (inverted sense: set = ignore pan) + value bar 0..64 |
| `PPC` | 0x17 | Panning: Pitch-Pan Center (note-name custom field) |
| `PPS` | 0x16 | Panning: Pitch-Pan Separation bar −32..32 (signed byte) |
| `RP` | 0x1B | Panning: Pan Swing bar 0..64 |
| `IFC` | 0x3A | Pitch: Default Cutoff bar 0..127 |
| `IFR` | 0x3B | Pitch: Default Resonance bar 0..127 |
| `MCh` | 0x3C | Pitch: MIDI Channel 0..17 |
| `MPr` | 0x3D | Pitch: MIDI Program −1..127 (0xFF = none) |
| `MIDIBnk` | 0x3E/0x3F | Pitch: MIDI Bank Low/High −1..127 |
| `VEnvelope` | 0x130 | Volume tab envelope block |
| `PEnvelope` | 0x182 | Panning tab envelope block |
| `PtEnvelope` | 0x1D4 | Pitch tab envelope block |

### Envelope (existing — `env_t`, one per tab)

| Field | UI binding |
|---|---|
| `Flags` bit 1 | "Volume/Panning/Frequency Envelope" On/Off toggle |
| `Flags` bit 2 | "Envelope Loop" toggle |
| `Flags` bit 4 | "Sustain Loop" toggle |
| `Flags` bit 8 | "Carry" toggle |
| `Flags` bit 0x80 | Pitch tab only: envelope-is-filter mode (per `IT_I.ASM`) |
| `Num` | node count (1..25); bounds all node ops |
| `LpB`/`LpE` | Loop Begin/End type-13 numeric fields; invariant `LpB ≤ LpE < Num` |
| `SLB`/`SLE` | SusLoop Begin/End; invariant `SLB ≤ SLE < Num` |
| `NodePoints[25]` | `(Magnitude, Tick)`; ticks strictly increasing; Magnitude 0..64 (vol) or −32..32 (pan/pitch); node 0 tick = 0 |

### Editor-local UI state (new, static in `it_editor.c`)

| State | Meaning |
|---|---|
| `InsTab` | active tab 0..3 (General/Volume/Panning/Pitch) = which object list builds |
| `EnvNodeSel` | selected node index on the active envelope (per `I_PreEnvelope`'s current-node) |
| `NoteWinTop`, `NoteWinSel` | note-translation window scroll/cursor (0..119) |

## Validation rules (from IT's own behaviour)

- Node count max 25; add beyond → rejected (status flash).
- Deleting below 2 nodes → rejected (IT keeps ≥2? verify in `I_PostEnvelope`;
  enforce whatever the proc does).
- Node ticks: node 0 fixed at tick 0; node i tick clamped to
  (tick[i−1]+1 .. tick[i+1]−1), max 9999 as in IT.
- Loop/susloop indices clamped to existing nodes; begin ≤ end (the type-13
  fields clamp on entry as `F_Post3Num` does).
- All mutations to fields the mixer reads (envelope data of a playing
  instrument) wrapped in `Engine_Lock`/`Engine_Unlock`.

## State transitions

Tab switch (`I_SelectScreen` equivalent): rebuilds the widget table for the new
object list; focus goes to the tab button (nav data in the object tables keeps
the original Tab/arrow order). In-progress numeric entry commits on blur, as the
existing framework already does.
